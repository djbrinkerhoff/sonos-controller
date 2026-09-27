#include "bsp/esp-bsp.h"
#include "network.hpp"
#include "screen.hpp"
#include "power.hpp"
#include "artwork.hpp"
#include "app.hpp"
#include "ui.hpp"
#include "fast_flush.hpp"
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
std::vector<Area> areas;
bool initialized=false;
bool storage_ok=true;
bool catalog_loaded=false;
// Speaker events arrive on the HTTP server task; they only nudge the worker.
std::atomic<bool> event_pending{false}, topology_event{false};
std::atomic<TickType_t> last_event{0};

void load_areas() {
    for(int i=0;i<8;++i) {
        auto data=setting(("area"+std::to_string(i)).c_str());
        if(data.empty()) continue;
        std::istringstream stream(data); Area area; std::getline(stream,area.name);
        std::string id; while(std::getline(stream,id)) if(!id.empty()) area.ids.push_back(id);
        if(!area.name.empty() && !area.ids.empty()) areas.push_back(std::move(area));
    }
}
void save_area(const Command& command) {
    if(command.name.empty() || command.name.size()>32 || command.name.find('\n')!=std::string::npos)
        throw std::runtime_error("Name the area using 1 to 32 characters");
    auto topology=client.rooms(command.room.ip);
    auto selected_room=std::find_if(topology.begin(),topology.end(),[&](const sonos::Room& r){return r.id==command.room.id;});
    if(selected_room==topology.end()) throw std::runtime_error("Selected room is unavailable");
    Area area; area.name=command.name;
    for(const auto& r:topology) if(r.coordinator==selected_room->coordinator) area.ids.push_back(r.id);
    auto found=std::find_if(areas.begin(),areas.end(),[&](const Area& a){return a.name==area.name;});
    size_t index=found==areas.end()?areas.size():static_cast<size_t>(found-areas.begin());
    if(index>=8) throw std::runtime_error("Eight areas are saved. Reuse a name to replace one.");
    std::string value=area.name;
    for(const auto& id:area.ids) value+='\n'+id;
    nvs_handle_t handle;
    if(nvs_open("controller",NVS_READWRITE,&handle)!=ESP_OK) throw std::runtime_error("Cannot save area");
    auto result=nvs_set_str(handle,("area"+std::to_string(index)).c_str(),value.c_str());
    if(result==ESP_OK) result=nvs_commit(handle);
    nvs_close(handle);
    if(result!=ESP_OK) throw std::runtime_error("Area could not be saved");
    if(index==areas.size()) areas.push_back(area); else areas[index]=area;
    ui_areas(areas);
    ui_toast("Saved "+area.name);
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
    favorites=client.favorites(seed);
    ui_rooms(found);
    ui_favorites(favorites);
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
    load_areas(); ui_areas(areas);
    auto boot=new Command; boot->action="Connect"; boot->ssid=setting("ssid"); boot->password=setting("password"); boot->seed=setting("seed");
    if(!boot->ssid.empty()) submit(boot);
    else { delete boot; ui_toast(storage_ok?"Connect to your home Wi-Fi to find your speakers.":"Settings storage failed; settings will not be saved.",!storage_ok); }
    TickType_t last_catalog_attempt=0, last_topology=0, last_battery=0, next_poll=0, last_rooms=0;
    int queue_track=-1; std::string queue_room;  // what the Queue view shows
    std::string art_uri="\x01", art_room;          // what the artwork panel shows
    std::vector<EventTarget> subscribed; TickType_t subscribed_at=0;
    size_t next_thumbnail=0;
    std::vector<size_t> thumbnail_retries;  // one more try for transient download failures
    bool was_online=false;
    for(;;) {
        // Sonos answers a new subscription with a full-state event, so an event
        // since subscribing proves events work and polling can relax.
        const bool events_live=subscribed_at && last_event.load()>=subscribed_at;
        const TickType_t now=xTaskGetTickCount();
        // Favorite artwork loads in the gaps between polls and commands.
        const bool thumbnails_pending=catalog_loaded && (next_thumbnail<favorites.size() || !thumbnail_retries.empty()) && !screen_off();
        const TickType_t wait=thumbnails_pending?0:(next_poll>now?next_poll-now:0);
        Command* raw=nullptr;
        xQueueReceive(commands,&raw,wait);
        std::unique_ptr<Command> c(raw);
        try {
            if(network_online()!=was_online) { was_online=!was_online; ui_online(was_online); }
            // A boot-time connect can fail while the router is down; the Wi-Fi
            // layer keeps retrying, so load the catalog once it comes back.
            if(!c && initialized && !catalog_loaded && network_online() &&
               xTaskGetTickCount()-last_catalog_attempt>pdMS_TO_TICKS(30000)) {
                last_catalog_attempt=xTaskGetTickCount();
                refresh(); next_thumbnail=0; thumbnail_retries.clear();
            }
            if(c) {
                const auto& a=c->action;
                if(!initialized) throw std::runtime_error("Wi-Fi hardware failed to initialize. Restart the controller.");
                if(a=="Connect") {
                    if(!c->seed.empty() && !sonos::valid_ipv4(c->seed)) throw std::runtime_error("Enter a valid speaker IP address");
                    network_connect(c->ssid,c->password); if(storage_ok) save_settings(*c); seed=c->seed;
                    refresh(); next_thumbnail=0; thumbnail_retries.clear();
                    ui_toast("Connected"); ui_show(View::NowPlaying);
                } else if(a=="Refresh") { refresh(); next_thumbnail=0; thumbnail_retries.clear(); }
                else if(a=="Favorite") client.play_favorite(c->room,c->favorite);
                else if(a=="Volume") client.volume(c->room,c->value);
                else if(a=="GroupVolume") client.volume(c->room,c->value,true);
                else if(a=="Mute") client.mute(c->room,c->value);
                else if(a=="GroupMute") client.mute(c->room,c->value,true);
                else if(a=="SaveArea") save_area(*c);
                else if(a=="Area" || a=="Group") {
                    client.apply_area(seed,c->area_ids);
                    ui_rooms(client.rooms(seed)); ui_select(c->area_ids.front());
                    if(a=="Area") ui_toast(c->name+" is grouped");
                    last_rooms=0;
                }
                else if(a=="Wake") client.invalidate_topology();
                else if(a=="Event") { event_pending=false; if(topology_event.exchange(false)) { client.invalidate_topology(); last_rooms=0; } }
                else if(a=="Queue") queue_track=-1;
                else if(a=="QueueTrack") { client.play_queue_track(c->room,c->value); queue_track=-1; }
                else if(a!="Poll") client.transport(c->room,a);
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
                client.invalidate_topology(); last_topology=xTaskGetTickCount();
            }
            if(!c && xTaskGetTickCount()<next_poll) {
                if(thumbnails_pending) {
                    const bool retry=next_thumbnail>=favorites.size();
                    const size_t index=retry?thumbnail_retries.back():next_thumbnail++;
                    if(retry) thumbnail_retries.pop_back();
                    const auto& f=favorites[index];
                    Artwork art;
                    if(!f.art.empty()) try { art=fetch_artwork(seed,thumbnail_url(f.art),art_spec::tile_side,art_spec::tile_radius,art_spec::background); }
                    catch(const std::exception& e) {
                        ESP_LOGW(TAG,"Favorite art for %s: %s",f.title.c_str(),e.what());
                        if(!retry) thumbnail_retries.push_back(index);
                    }
                    if(art.pixels) ui_favorite_art(f.id,std::move(art));
                }
                continue;
            }
            next_poll=xTaskGetTickCount()+pdMS_TO_TICKS(events_live?15000:4000);
            if(screen_off() && (!c || c->action=="Event")) continue; // nobody is looking; save the speakers the traffic
            const auto target=ui_selected();
            if(target.id.empty()) continue;
            const auto state=client.state(target);
            ui_state(target,state);
            const auto leader=client.coordinator(target);
            std::vector<EventTarget> targets{{leader.ip,"AVTransport"},{target.ip,"RenderingControl"},
                                             {leader.ip,"GroupRenderingControl"},{leader.ip,"ZoneGroupTopology"}};
            if(targets!=subscribed) { events_set_targets(targets); subscribed=targets; subscribed_at=xTaskGetTickCount(); }
            if(state.art!=art_uri || target.id!=art_room) {
                art_uri=state.art; art_room=target.id;
                Artwork art;
                if(!state.art.empty()) try { art=fetch_artwork(target.ip,state.art,art_spec::now_side,art_spec::now_radius,art_spec::background); }
                catch(const std::exception& e) { ESP_LOGW(TAG,"Artwork unavailable: %s",e.what()); }
                ui_artwork(target,std::move(art));
            }
            const auto view=ui_view();
            if(view==View::Queue && (state.track!=queue_track || target.id!=queue_room)) {
                // Show a window from just before the current track onwards.
                int total=0; const int start=std::max(0,state.track-3);
                auto items=client.queue(target,start,50,&total);
                ui_queue(target,items,total,state.track);
                queue_track=state.track; queue_room=target.id;
            }
            if(view==View::Rooms && (c || !last_rooms || xTaskGetTickCount()-last_rooms>pdMS_TO_TICKS(15000))) {
                // Room cards show what every group is playing: fresh topology,
                // then two calls per group coordinator.
                last_rooms=xTaskGetTickCount();
                auto all=client.rooms(seed);
                ui_rooms(all);
                std::vector<std::pair<std::string,sonos::Summary>> summaries;
                for(const auto& r:all) if(r.id==r.coordinator) {
                    try { summaries.emplace_back(r.id,client.summary(r)); }
                    catch(const std::exception& e) { ESP_LOGW(TAG,"Summary for %s: %s",r.name.c_str(),e.what()); }
                }
                ui_summaries(summaries);
            }
        } catch(const std::exception& e) {
            // Drop queued actions after any failure; never replay a possibly completed queue mutation.
            Command* pending=nullptr;
            while(xQueueReceive(commands,&pending,0)==pdTRUE) delete pending;
            event_pending=false;
            client.invalidate_topology();
            ui_toast(e.what(),true); ESP_LOGE(TAG,"Controller operation failed: %s",e.what());
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
    await_touch_controller();
    // Measured: a full-screen PSRAM draw buffer (one band) renders Favorites in
    // ~154 ms versus ~69 ms for the BSP's 50-line bands in internal RAM, so the
    // default stays. Pixel throughput, not per-band overhead, is the cost here.
    auto display=bsp_display_start();
    if(!display) {ESP_LOGE(TAG,"Display initialization failed");return;}
    bsp_display_backlight_on();
    commands=xQueueCreate(8,sizeof(Command*));
    if(!commands) return;
    bsp_display_lock(0);
    lv_display_set_rotation(display,LV_DISPLAY_ROTATION_90);
    fast_flush_install(display);
    ui_build();
    screen_init(screen_woke);
    bsp_display_unlock();
    if(xTaskCreate(worker,"sonos",24576,nullptr,4,nullptr)!=pdPASS) ui_toast("Unable to start the controller task",true);
}
