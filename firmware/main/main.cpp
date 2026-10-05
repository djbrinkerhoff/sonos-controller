#include "bsp/esp-bsp.h"
#include "network.hpp"
#include "screen.hpp"
#include "power.hpp"
#include "artwork.hpp"
#include "app.hpp"
#include "ui.hpp"
#include "fast_flush.hpp"
#include "display_power.hpp"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "events.hpp"
#include "ota.hpp"
#include "debug.hpp"
#include "sonos.hpp"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_lcd_touch_gt911.h"
#ifdef CONFIG_TAB5_I2C_SCAN
#include "esp_lvgl_port.h"
#endif
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sstream>
#ifdef CONFIG_TAB5_I2C_SCAN
#include <cstdio>
#endif

namespace {
const char* TAG="controller";
QueueHandle_t commands;
sonos::Client client(soap_http);
std::string seed;
std::vector<sonos::Favorite> favorites;  // the worker's copy, for artwork thumbnails
std::vector<sonos::Room> published;      // the room list last pushed to the UI
bool initialized=false;
bool storage_ok=true;
bool catalog_loaded=false;
// Speaker events arrive on the HTTP server task; they only nudge the worker.
std::atomic<bool> event_pending{false}, topology_event{false}, content_event{false};
std::atomic<TickType_t> last_event{0};

bool same_rooms(const std::vector<sonos::Room>& a,const std::vector<sonos::Room>& b) {
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i)
        if(a[i].id!=b[i].id || a[i].ip!=b[i].ip || a[i].coordinator!=b[i].coordinator || a[i].name!=b[i].name) return false;
    return true;
}
bool same_favorites(const std::vector<sonos::Favorite>& a,const std::vector<sonos::Favorite>& b) {
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i)
        if(a[i].id!=b[i].id || a[i].title!=b[i].title || a[i].art!=b[i].art || a[i].uri!=b[i].uri || a[i].radio!=b[i].radio) return false;
    return true;
}
// Pushes a room list to the UI only when it actually changed: rebuilding the
// room cards re-renders the screen, so polling must not call it blindly.
void push_rooms(const std::vector<sonos::Room>& all) {
    if(!same_rooms(all,published)) { published=all; ui_rooms(all); }
}

// Starting a favorite in the startup room from silence uses a set level
// (CONFIG_TAB5_START_VOLUME); never change the volume under music already playing.
void apply_start_volume(const sonos::Room& room) {
    if(CONFIG_TAB5_START_VOLUME<=0 || room.name!=CONFIG_TAB5_DEFAULT_ROOM) return;
    if(client.summary(client.coordinator(room)).playback=="PLAYING") return;
    client.volume(room,CONFIG_TAB5_START_VOLUME);
}
void save_settings(const Command& command) {
    nvs_handle_t handle;
    if(nvs_open("controller",NVS_READWRITE,&handle)!=ESP_OK) throw std::runtime_error("Cannot save Wi-Fi settings");
    auto result=nvs_set_str(handle,"ssid",command.ssid.c_str());
    if(result==ESP_OK) result=nvs_set_str(handle,"password",command.password.c_str());
    if(result==ESP_OK) result=nvs_set_str(handle,"seed",command.seed.c_str());
    if(result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    if(result!=ESP_OK) throw std::runtime_error("Settings could not be saved");
}
// Every visible room's IP from the last good topology, so a stale seed or a
// powered-off speaker does not strand the controller; SSDP is the last resort.
std::vector<std::string> known_speakers() {
    std::istringstream stream(setting("speakers")); std::vector<std::string> ips; std::string ip;
    while(std::getline(stream,ip)) if(sonos::valid_ipv4(ip)) ips.push_back(ip);
    return ips;
}
void save_known_speakers(const std::vector<sonos::Room>& found) {
    std::string value;
    for(size_t i=0;i<found.size() && i<32;++i) value+=(i?"\n":"")+found[i].ip;
    if(value.empty() || value==setting("speakers")) return; // spare flash writes
    nvs_handle_t handle;
    if(nvs_open("controller",NVS_READWRITE,&handle)!=ESP_OK) return;
    if(nvs_set_str(handle,"speakers",value.c_str())==ESP_OK) nvs_commit(handle);
    nvs_close(handle);
}
// Runs in the LVGL task. Sleep may have hidden external changes, so fetch
// fresh topology and state straight away.
void screen_woke() { auto c=new Command; c->action="Wake"; submit(c); }
void speaker_event(const char* service) {
    last_event=xTaskGetTickCount();
    if(!std::strcmp(service,"ZoneGroupTopology")) topology_event=true;
    if(!std::strcmp(service,"ContentDirectory")) content_event=true;
    if(event_pending.exchange(true)) return; // one refresh covers a burst
    auto c=new Command; c->action="Event";
    if(!submit(c)) event_pending=false;
}
void refresh() {
    if(!network_online()) throw std::runtime_error("Wi-Fi is disconnected");
    std::vector<std::string> candidates;
    for(const auto& ip:{seed,setting("seed")}) if(!ip.empty()) candidates.push_back(ip);
    for(const auto& ip:known_speakers()) candidates.push_back(ip);
    std::vector<sonos::Room> found;
    std::vector<std::string> tried;
    for(const auto& ip:candidates) {
        if(std::find(tried.begin(),tried.end(),ip)!=tried.end()) continue;
        tried.push_back(ip);
        if(!speaker_reachable(ip,1500)) continue;
        try { found=client.rooms(ip); seed=ip; break; }
        catch(const std::exception& e) { ESP_LOGW(TAG,"Speaker %s unusable: %s",ip.c_str(),e.what()); }
    }
    const bool discovered=found.empty();
    if(discovered) { seed=discover_speaker(); found=client.rooms(seed); }
    ESP_LOGI(TAG,"Using speaker %s (%s), %u rooms",seed.c_str(),discovered?"discovered":"stored",static_cast<unsigned>(found.size()));
    save_known_speakers(found);
    // Reconnects (standby wake, Wi-Fi blips) land here too. Republishing an
    // unchanged list would rebuild every tile and refetch all covers.
    auto fresh=client.favorites(seed);
    push_rooms(found);
    if(!catalog_loaded || !same_favorites(fresh,favorites)) { favorites=std::move(fresh); ui_favorites(favorites); }
    catalog_loaded=true;
    ota_mark_healthy(); // a new image proves itself by reaching the speakers
}
// Favorite tiles only need ~200 px art. Apple's image CDN serves any size by
// file name; "cc" centre-crops to a square, where "bb" would letterbox the
// 4:1 banners editorial playlists use. imgix (Sonos Radio) serves progressive
// JPEGs, which the P4's hardware decoder cannot read, and its existing
// auto=format overrides fm, so the whole query is replaced to get a PNG.
std::string thumbnail_url(const std::string& uri) {
    if(uri.find("mzstatic.com/")!=std::string::npos) {
        auto slash=uri.rfind('/'); auto name=uri.substr(slash+1);
        if(name.find('x')!=std::string::npos && name.find('.')!=std::string::npos) return uri.substr(0,slash+1)+"400x400cc.jpg";
    }
    if(uri.find("imgix.net/")!=std::string::npos) return uri.substr(0,uri.find('?'))+"?w=200&h=200&fit=crop&fm=png";
    return uri;
}

// --- artwork ---------------------------------------------------------------
// Downloads and decodes on their own low-priority task: a slow cover must not
// delay pause or volume behind it on the command worker. Now-playing art has
// a depth-1 slot (only the newest matters); favorite tiles share a small FIFO
// and anything that waited too long is dropped before a fetch starts.
struct ArtJob {
    sonos::Room room;          // now-playing target; ignored for tile jobs
    std::string ip, uri;       // fetch source and requested art
    bool now=false;            // now-playing slot vs tile FIFO
    size_t tile=0;             // tile jobs: index into the favorites list
    std::string fav_id;        // tile jobs: expected favorite id
    int64_t queued=0;          // esp_timer time the job was posted
};
QueueHandle_t now_jobs=nullptr, tile_jobs=nullptr;
constexpr int MAX_ART_SENDS=4;  // now-playing fetches per art URI before giving up

void post_now_art(const sonos::Room& target,const std::string& uri) {
    if(!now_jobs) return;
    ArtJob* stale=nullptr;
    xQueueReceive(now_jobs,&stale,0); delete stale;  // only the newest matters
    auto job=new ArtJob; job->now=true; job->room=target; job->ip=target.ip; job->uri=uri; job->queued=esp_timer_get_time();
    if(xQueueSend(now_jobs,&job,0)!=pdTRUE) delete job;
}
void post_tile_art(size_t index,const std::string& id,const std::string& uri) {
    if(!tile_jobs || uri.empty()) return;
    auto job=new ArtJob; job->tile=index; job->fav_id=id; job->uri=uri; job->ip=seed; job->queued=esp_timer_get_time();
    if(xQueueSend(tile_jobs,&job,0)!=pdTRUE) delete job;
}
void artwork_worker(void*) {
    auto set=xQueueCreateSet(9);
    if(set) { xQueueAddToSet(now_jobs,set); xQueueAddToSet(tile_jobs,set); }
    for(;;) {
        ArtJob* job=nullptr;
        if(set) {
            auto member=xQueueSelectFromSet(set,portMAX_DELAY);
            if(member) xQueueReceive(member,&job,0);
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
            if(xQueueReceive(now_jobs,&job,0)!=pdTRUE) xQueueReceive(tile_jobs,&job,0);
        }
        if(!job) continue;
        std::unique_ptr<ArtJob> held(job);
        // Tile art only matters while someone is looking; jobs older than
        // 20 s describe scroll positions the user already left.
        if((!job->now && screen_off()) || esp_timer_get_time()-job->queued>20*1000000LL) continue;
        Artwork art;
        try {
            art=fetch_artwork(job->ip,job->uri,job->now?art_spec::now_side:art_spec::tile_side,
                              job->now?art_spec::now_radius:art_spec::tile_radius,art_spec::background);
        } catch(const std::exception& e) { ESP_LOGW(TAG,"artwork: %s",e.what()); }
        if(job->now) {
            const bool ok=art.pixels!=nullptr;
            ui_artwork(job->room,std::move(art));
            // Reports back for retry bookkeeping; a dropped report just means
            // one extra fetch of the same cover.
            auto done=new Command; done->action="ArtDone"; done->room=job->room;
            done->name=job->uri; done->value=ok?1:0;
            submit(done);
        } else if(art.pixels) ui_favorite_art(job->fav_id,job->tile,std::move(art));
    }
}

void worker(void*) {
    try {
        if(bsp_feature_enable(BSP_FEATURE_WIFI,true)!=ESP_OK) throw std::runtime_error("Cannot power the Wi-Fi module");
        power_init(); // must follow the Wi-Fi power-on, which resets the charger pins
        motion_start(screen_note_motion);
        vTaskDelay(pdMS_TO_TICKS(300));
        network_init(); initialized=true;
        events_start(speaker_event);
        if(auto server=events_server()) { ota_register(server); debug_register(server); }
    }
    catch(const std::exception& e) { ui_toast(e.what(),true); ESP_LOGE(TAG,"Startup failed: %s",e.what()); }
    auto boot=new Command; boot->action="Connect"; boot->ssid=setting("ssid"); boot->password=setting("password"); boot->seed=setting("seed");
    if(!boot->ssid.empty()) submit(boot);
    else { delete boot; ui_toast(storage_ok?"Connect to your home Wi-Fi to find your speakers.":"Settings storage failed; settings will not be saved.",!storage_ok); }
    TickType_t last_catalog_attempt=0, last_topology=0, last_battery=0, next_poll=0, last_rooms=0;
    TickType_t last_queue=0, last_catalog_check=0, last_rediscovery=0;
    int queue_track=-1; std::string queue_room;  // what the Queue view shows
    bool catalog_due=false, topology_stale=true;
    unsigned failures=0;
    std::string art_uri="\x01", art_room;          // what the artwork panel should show
    unsigned art_sent=0; TickType_t art_next_try=0; bool art_done=false;
    std::vector<EventTarget> subscribed; TickType_t subscribed_at=0;
    bool was_online=false, radio_saving=false;
    // Standby follows CONFIG_TAB5_STANDBY_MINUTES with the screen off and the
    // selected room not playing. Unknown counts as playing.
    bool playing=true; TickType_t quiet_since=0, last_playing_check=0;
    auto check_playing=[&]{
        last_playing_check=xTaskGetTickCount();
        const auto target=ui_selected();
        if(target.id.empty()) { playing=false; return; }
        const auto summary=client.summary(client.coordinator(target));
        playing=summary.playback=="PLAYING" || summary.playback=="TRANSITIONING";
    };
    // Queued commands preempt the multi-request background reads below.
    client.set_interrupt([]{ return uxQueueMessagesWaiting(commands)>0; });
    auto invalidate=[&]{ client.invalidate_topology(); topology_stale=true; };
    for(;;) {
        // Sonos answers a new subscription with a full-state event, so an event
        // since subscribing proves events work and polling can relax.
        const bool events_live=subscribed_at && last_event.load()>=subscribed_at;
        const TickType_t now=xTaskGetTickCount();
        const TickType_t wait=next_poll>now?next_poll-now:0;
        Command* raw=nullptr;
        xQueueReceive(commands,&raw,wait);
        std::unique_ptr<Command> c(raw);
        try {
            if(network_online()!=was_online) { was_online=!was_online; ui_online(was_online); }
            if(screen_off()!=radio_saving) { radio_saving=!radio_saving; network_power_save(radio_saving); }
            if(!screen_off() || playing) quiet_since=0;
            else if(!quiet_since) quiet_since=xTaskGetTickCount();
            if(!c && quiet_since && CONFIG_TAB5_STANDBY_MINUTES>0 && display_asleep() &&
               xTaskGetTickCount()-quiet_since>=pdMS_TO_TICKS(CONFIG_TAB5_STANDBY_MINUTES*60000)) {
                ESP_LOGI(TAG,"Nothing playing for %d minutes; standby",CONFIG_TAB5_STANDBY_MINUTES);
                network_suspend();
                const char* woken=power_standby();
                network_resume();
                ESP_LOGI(TAG,"Standby ended (%s); reconnecting",woken);
                // Taps queue meanwhile; polling before the network is back would only fail.
                for(int i=0;i<150 && !network_online();++i) vTaskDelay(pdMS_TO_TICKS(100));
                quiet_since=0; playing=true;
                subscribed.clear();       // subscriptions lapsed while offline
                invalidate();
                next_poll=0;
                continue;
            }
            // A boot-time connect can fail while the router is down; the Wi-Fi
            // layer keeps retrying, so load the catalog once it comes back.
            if(!c && initialized && !catalog_loaded && network_online() &&
               xTaskGetTickCount()-last_catalog_attempt>pdMS_TO_TICKS(30000)) {
                last_catalog_attempt=xTaskGetTickCount();
                refresh(); last_catalog_check=xTaskGetTickCount();
            }
            if(c) {
                const auto& a=c->action;
                if(!initialized) throw std::runtime_error("Wi-Fi hardware failed to initialize. Restart the controller.");
                if(a=="Connect") {
                    if(!c->seed.empty() && !sonos::valid_ipv4(c->seed)) throw std::runtime_error("Enter a valid speaker IP address");
                    network_connect(c->ssid,c->password); if(storage_ok) save_settings(*c); seed=c->seed;
                    refresh(); catalog_due=false; last_catalog_check=xTaskGetTickCount();
                    ui_toast("Connected"); ui_show(View::NowPlaying);
                } else if(a=="Refresh") { refresh(); catalog_due=false; last_catalog_check=xTaskGetTickCount(); }
                else if(a=="Favorite") { apply_start_volume(c->room); client.play_favorite(c->room,c->favorite); }
                else if(a=="TileArt") {
                    // Verify the index and id together; the list may have been
                    // reloaded since the UI asked.
                    const size_t index=static_cast<size_t>(c->value);
                    if(index<favorites.size() && favorites[index].id==c->name)
                        post_tile_art(index,c->name,thumbnail_url(favorites[index].art));
                }
                else if(a=="ArtDone") {
                    // Retry bookkeeping for the Now Playing cover; failures
                    // re-enter through the deadline below, so a dropped report
                    // costs at most one extra fetch.
                    if(c->room.id==art_room && c->name==art_uri) art_done=c->value!=0 || art_sent>=MAX_ART_SENDS;
                }
                else if(a=="Volume" || a=="GroupVolume") {
                    const bool group=a=="GroupVolume";
                    client.volume(c->room,c->value,group);  // level first, so unmuting never plays the old one
                    if(c->unmute) client.mute(c->room,false,group);
                }
                else if(a=="Mute") client.mute(c->room,c->value);
                else if(a=="GroupMute") client.mute(c->room,c->value,true);
                else if(a=="Wake") invalidate();
                else if(a=="Event") {
                    event_pending=false;
                    if(topology_event.exchange(false)) { invalidate(); last_rooms=0; }
                    // Queue and favorites can both change through another app.
                    if(content_event.exchange(false)) { queue_track=-1; catalog_due=true; }
                }
                else if(a=="Queue") queue_track=-1;
                else if(a=="QueueTrack") { client.play_queue_track(c->room,c->value); queue_track=-1; }
                else if(a!="Poll") client.transport(c->room,a);
                failures=0;  // a successful command proves the speakers are reachable
            }
            if(!last_battery || xTaskGetTickCount()-last_battery>pdMS_TO_TICKS(30000)) {
                last_battery=xTaskGetTickCount();
                const auto battery=battery_read();
                ui_battery(battery);
                ESP_LOGI(TAG,"Battery: %s %d mV %d%% %d mA%s",battery.present?"present":"absent",battery.pack_mv,
                         battery.percent,battery.current_ma,battery.charging?" charging":"");
            }
            // External regrouping is picked up within 30 s even without events.
            if(xTaskGetTickCount()-last_topology>pdMS_TO_TICKS(30000)) {
                invalidate(); last_topology=xTaskGetTickCount();
            }
            if(!c && xTaskGetTickCount()<next_poll) continue;
            next_poll=xTaskGetTickCount()+pdMS_TO_TICKS(events_live?15000:4000);
            if(screen_off() && (!c || c->action=="Event")) {
                // Nobody is looking: skip the refresh, but note whether the
                // room still plays (events, or every 5 minutes) for standby.
                if(c || xTaskGetTickCount()-last_playing_check>pdMS_TO_TICKS(300000)) check_playing();
                continue;
            }
            const auto target=ui_selected();
            if(target.id.empty()) continue;
            const auto state=client.state(target);
            ui_state(target,state);
            playing=state.playback=="PLAYING" || state.playback=="TRANSITIONING";
            failures=0;  // a successful refresh proves the speakers are reachable
            const auto leader=client.coordinator(target);
            std::vector<EventTarget> targets{{leader.ip,"AVTransport"},{target.ip,"RenderingControl"},
                                             {leader.ip,"GroupRenderingControl"},{leader.ip,"ZoneGroupTopology"},
                                             {leader.ip,"ContentDirectory"}};
            if(targets!=subscribed) { events_set_targets(targets); subscribed=targets; subscribed_at=xTaskGetTickCount(); }
            // Regrouping also decides room vs group controls on Now Playing,
            // so topology changes are published no matter which view is open.
            std::vector<sonos::Room> fresh_rooms;
            if(topology_stale) {
                fresh_rooms=client.rooms(seed);
                topology_stale=false;
                push_rooms(fresh_rooms);
            }
            // Track changes queue a fetch on the artwork task; a transient
            // failure is retried with widening gaps, bounded per URI.
            if(state.art!=art_uri || target.id!=art_room) {
                art_uri=state.art; art_room=target.id;
                art_done=art_uri.empty(); art_sent=0; art_next_try=0;
                if(art_uri.empty()) ui_artwork(target,Artwork{});
            }
            if(!art_done && art_sent<MAX_ART_SENDS && xTaskGetTickCount()>=art_next_try) {
                post_now_art(target,art_uri);
                ++art_sent;
                art_next_try=xTaskGetTickCount()+pdMS_TO_TICKS(art_sent==1?10000:art_sent==2?30000:60000);
            }
            const auto view=ui_view();
            // External edits leave stale rows while the track stays put, so
            // the window reloads on content events and every 60 s.
            if(view==View::Queue && (state.track!=queue_track || target.id!=queue_room ||
                                     xTaskGetTickCount()-last_queue>pdMS_TO_TICKS(60000))) {
                // Show a window from just before the current track onwards.
                int total=0; const int start=std::max(0,state.track-3);
                auto items=client.queue(target,start,50,&total);
                ui_queue(target,items,total,state.track);
                queue_track=state.track; queue_room=target.id; last_queue=xTaskGetTickCount();
            }
            if(view==View::Rooms && (c || !last_rooms || xTaskGetTickCount()-last_rooms>pdMS_TO_TICKS(15000))) {
                // Room cards show what every group is playing: fresh topology,
                // then two calls per group coordinator.
                last_rooms=xTaskGetTickCount();
                auto all=fresh_rooms.empty()?client.rooms(seed):fresh_rooms;
                push_rooms(all);
                std::vector<std::pair<std::string,sonos::Summary>> summaries;
                for(const auto& r:all) if(r.id==r.coordinator) {
                    // Queued commands outrank room summaries; the next pass
                    // fills in the rest.
                    if(uxQueueMessagesWaiting(commands)) break;
                    try { summaries.emplace_back(r.id,client.summary(r)); }
                    catch(const std::exception& e) { ESP_LOGW(TAG,"Summary for %s: %s",r.name.c_str(),e.what()); }
                }
                ui_summaries(summaries);
            }
            // Favorites change through the Sonos app too: recheck on content
            // events and every 30 minutes, pushing only real changes.
            if(catalog_loaded &&
               (catalog_due || xTaskGetTickCount()-last_catalog_check>pdMS_TO_TICKS(30*60000)) &&
               xTaskGetTickCount()-last_catalog_check>pdMS_TO_TICKS(60000)) {
                last_catalog_check=xTaskGetTickCount(); catalog_due=false;
                try {
                    auto fresh_favorites=client.favorites(seed);
                    if(!same_favorites(fresh_favorites,favorites)) { favorites=fresh_favorites; ui_favorites(fresh_favorites); }
                } catch(const sonos::Preempted&) {
                    // A queued command stopped the check; it runs again later.
                } catch(const std::exception& e) { ESP_LOGW(TAG,"Favorites check: %s",e.what()); }
            }
        } catch(const sonos::Preempted&) {
            // A queued command stopped a background read; it runs next.
        } catch(const std::exception& e) {
            // Drop queued actions after any failure; never replay a possibly completed queue mutation.
            Command* pending=nullptr;
            while(xQueueReceive(commands,&pending,0)==pdTRUE) delete pending;
            event_pending=false;
            invalidate();
            ui_toast(e.what(),true); ESP_LOGE(TAG,"Controller operation failed: %s",e.what());
            // Repeated failures mean the stored speaker addresses are probably
            // stale; rediscover, keeping the selection by room id. Bounded to
            // one attempt a minute so a dead speaker cannot stall the worker.
            if(++failures>=5 && initialized && network_online() &&
               xTaskGetTickCount()-last_rediscovery>pdMS_TO_TICKS(60000)) {
                last_rediscovery=xTaskGetTickCount();
                try {
                    ESP_LOGW(TAG,"%u failed operations; rediscovering speakers",failures);
                    refresh(); failures=0;
                } catch(const std::exception& again) { ESP_LOGW(TAG,"Rediscovery: %s",again.what()); }
            }
        }
    }
}
}
bool submit(Command* command) {
    if(commands && xQueueSend(commands,&command,0)==pdTRUE) return true;
    delete command;
    return false;
}
std::string setting(const char* key) {
    nvs_handle_t handle;
    if(nvs_open("controller",NVS_READONLY,&handle)!=ESP_OK) return "";
    size_t len=0; std::string value;
    if(nvs_get_str(handle,key,nullptr,&len)==ESP_OK && len<=4096) {
        value.resize(len); nvs_get_str(handle,key,value.data(),&len);
        if(!value.empty()) value.pop_back();
    }
    nvs_close(handle); return value;
}
#ifdef CONFIG_TAB5_I2C_SCAN
// Diagnostic only: enumerate the BSP I2C bus so the panel revision can be
// identified from the hardware instead of from the rear label. Runs under the
// same power sequence bsp_display_start() uses, so the result reflects the
// state the real driver would probe in.
static void i2c_scan_pass(const char* label, i2c_master_bus_handle_t bus) {
    int hits = 0;
    char line[128];
    int n = snprintf(line, sizeof line, "SCAN %s:", label);
    for (uint16_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%02x", addr);
            ++hits;
        }
    }
    if (!hits) snprintf(line + n, sizeof(line) - n, " (none)");
    ESP_LOGI(TAG, "%s", line);
}

static void i2c_scan_diagnostic() {
    ESP_LOGI(TAG, "=== I2C scan diagnostic (TAB5_I2C_SCAN=y) ===");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) { ESP_LOGE(TAG, "I2C bus handle unavailable"); return; }
    ESP_LOGI(TAG, "bus ready, SDA=%d SCL=%d", BSP_I2C_SDA, BSP_I2C_SCL);

    i2c_scan_pass("baseline", bus);

    // Match bsp_display_new_with_handles() one call at a time, probing the
    // touch address after each, to find which step loses the controller.
    auto probe55 = [&](const char* when) {
        esp_err_t r = i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 100);
        ESP_LOGI(TAG, "  0x55 @ %-22s -> %s", when, esp_err_to_name(r));
    };

    probe55("start");
    if (bsp_feature_enable(BSP_FEATURE_LCD, true) != ESP_OK)
        ESP_LOGE(TAG, "LCD feature enable failed");
    probe55("after LCD enable");
    if (bsp_display_brightness_init() != ESP_OK)
        ESP_LOGE(TAG, "brightness init failed");
    probe55("after brightness init");
    if (bsp_feature_enable(BSP_FEATURE_TOUCH, true) != ESP_OK)
        ESP_LOGE(TAG, "Touch feature enable failed");
    probe55("after TOUCH enable");
    vTaskDelay(pdMS_TO_TICKS(500));
    probe55("after 500ms settle");

    i2c_scan_pass("after LCD+bright+TOUCH", bus);

    for (uint16_t addr : {ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS,
                          ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP}) {
        esp_err_t r = i2c_master_probe(bus, addr, 200);
        ESP_LOGI(TAG, "BSP touch probe 0x%02x -> %s", addr, esp_err_to_name(r));
    }

    // bsp_display_start_with_config() runs lvgl_port_init() *before* the panel
    // init that performs the touch probe. Repeat the probe after that call to
    // see whether LVGL port init is what takes the bus down.
    {
        lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
        esp_err_t r = lvgl_port_init(&lvgl_cfg);
        ESP_LOGI(TAG, "lvgl_port_init -> %s", esp_err_to_name(r));
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    i2c_scan_pass("after lvgl_port_init", bus);
    {
        esp_err_t r = i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 200);
        ESP_LOGI(TAG, "touch probe 0x55 after lvgl_port_init -> %s", esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "=== scan complete; UI not started ===");
}
#endif

// bsp_display_start() probes the touch controller to identify the panel, and
// on ST7121/ST7123 units the responder at 0x55 is the TDDI itself, which is
// briefly offline while the shared reset on the IO expander is released. The
// probe can then miss entirely, and bsp_get_board_version() answers that with
// assert(NULL) - a reboot loop we cannot retry our way out of, because it is a
// raw libc assert rather than a BSP_ERROR_CHECK.
//
// So release the resets first and wait for the controller to actually answer,
// rather than guessing a fixed delay: no published settle time exists, and the
// 500 ms the BSP allows is hard-coded with no Kconfig override. Polling for the
// real ACK adapts to whichever panel revision is fitted.
//
// Upstream tracking: espressif/esp-bsp issue #829 and PR #830 (one-line fix
// that releases the reset before detection), both open as of this writing.
static void await_touch_controller() {
    bsp_i2c_get_handle();
    bsp_feature_enable(BSP_FEATURE_LCD, true);
    bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    for (int attempt = 1; attempt <= 40; ++attempt) {
        if (i2c_master_probe(bsp_i2c_get_handle(), ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 100) == ESP_OK ||
            i2c_master_probe(bsp_i2c_get_handle(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
            ESP_LOGI(TAG, "Touch controller answered after %d attempt(s)", attempt);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "Touch controller did not answer; display detection may fail");
}

extern "C" void app_main() {
    // Never erase on failure: that would destroy saved settings. Boot without
    // persistence instead, so the device does not look bricked.
    debug_log_init();
    auto nvs=nvs_flash_init();
    if(nvs!=ESP_OK) { ESP_LOGE(TAG,"NVS init failed: %s (settings preserved)",esp_err_to_name(nvs)); storage_ok=false; }
#ifdef CONFIG_TAB5_FORGET_SPEAKERS
    // Diagnostic only: drop stored speaker addresses so every boot has to
    // find a speaker through discovery. Wi-Fi credentials are kept.
    if(nvs_handle_t handle; storage_ok && nvs_open("controller",NVS_READWRITE,&handle)==ESP_OK) {
        for(const char* key:{"seed","speakers"}) {
            auto erased=nvs_erase_key(handle,key);
            ESP_LOGW(TAG,"Forget speakers: %s -> %s",key,esp_err_to_name(erased));
        }
        nvs_commit(handle); nvs_close(handle);
    }
#endif
#ifdef CONFIG_TAB5_I2C_SCAN
    i2c_scan_diagnostic();
    return;
#endif
    ota_log_boot();
    ESP_LOGI(TAG,"Reset reason %d",static_cast<int>(esp_reset_reason()));
    await_touch_controller();
    // Measured: a full-screen PSRAM draw buffer (one band) renders Favorites in
    // ~154 ms versus ~69 ms for the BSP's 50-line bands in internal RAM, so the
    // default stays. Pixel throughput, not per-band overhead, is the cost here.
    auto display=bsp_display_start();
    if(!display) {ESP_LOGE(TAG,"Display initialization failed");return;}
    bsp_display_backlight_on();
    commands=xQueueCreate(8,sizeof(Command*));
    now_jobs=xQueueCreate(1,sizeof(ArtJob*));
    tile_jobs=xQueueCreate(8,sizeof(ArtJob*));
    if(!commands || !now_jobs || !tile_jobs) return;
    bsp_display_lock(0);
    lv_display_set_rotation(display,LV_DISPLAY_ROTATION_90);
    fast_flush_install(display);
    display_power_init(display);
    ui_build();
    screen_init(screen_woke);
    bsp_display_unlock();
    // Full speed while the panel driver holds its lock (screen on); 40 MHz
    // once display_sleep() deletes the driver. Measured: 9 mA at the pack.
    esp_pm_config_t pm={}; pm.max_freq_mhz=CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ; pm.min_freq_mhz=40;
    if(auto result=esp_pm_configure(&pm); result!=ESP_OK) ESP_LOGW(TAG,"Frequency scaling unavailable: %s",esp_err_to_name(result));
    if(xTaskCreate(worker,"sonos",24576,nullptr,4,nullptr)!=pdPASS) ui_toast("Unable to start the controller task",true);
    // Lower priority than the command worker; HTTPS needs a generous stack.
    if(xTaskCreate(artwork_worker,"artwork",12288,nullptr,3,nullptr)!=pdPASS) ui_toast("Unable to start the artwork task",true);
}
