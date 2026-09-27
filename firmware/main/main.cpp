#include "bsp/esp-bsp.h"
#include "network.hpp"
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
#include <memory>
#include <stdexcept>
#include <sstream>
#ifdef CONFIG_TAB5_I2C_SCAN
#include <cstdio>
#endif

namespace {
const char* TAG="controller";
struct Command {
    std::string action, ssid, password, seed;
    sonos::Room room, destination;
    sonos::Favorite favorite;
    int value=0;
    std::vector<std::string> area_ids;
};
QueueHandle_t commands;
sonos::Client client(soap_http);
std::vector<sonos::Room> rooms;
std::vector<sonos::Favorite> favorites;
sonos::Room selected;
sonos::State current;
std::string seed;
lv_obj_t *status_label,*room_select,*favorite_list,*title_label,*artist_label,*volume_slider,*volume_label;
lv_obj_t *ssid_input,*password_input,*seed_input,*keyboard,*join_select,*tabs,*play_button,*next_button,*prev_button,*mute_button;
lv_obj_t *group_slider,*group_label,*area_name,*area_list;
struct Area {std::string name; std::vector<std::string> ids;};
std::vector<Area> areas;
bool initialized=false;
bool storage_ok=true;
bool catalog_loaded=false;

struct DisplayLock { DisplayLock(){bsp_display_lock(0);} ~DisplayLock(){bsp_display_unlock();} };
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
void load_areas() {
    for(int i=0;i<8;++i) {
        auto data=setting(("area"+std::to_string(i)).c_str());
        if(data.empty()) continue;
        std::istringstream stream(data); Area area; std::getline(stream,area.name);
        std::string id; while(std::getline(stream,id)) if(!id.empty()) area.ids.push_back(id);
        if(!area.name.empty() && !area.ids.empty()) areas.push_back(std::move(area));
    }
}
void render_areas();
void save_area(const Command& command) {
    if(command.ssid.empty() || command.ssid.size()>32 || command.ssid.find('\n')!=std::string::npos)
        throw std::runtime_error("Name the area using 1 to 32 characters");
    auto topology=client.rooms(command.room.ip);
    auto selected_room=std::find_if(topology.begin(),topology.end(),[&](const sonos::Room& r){return r.id==command.room.id;});
    if(selected_room==topology.end()) throw std::runtime_error("Selected room is unavailable");
    Area area; area.name=command.ssid;
    for(const auto& r:topology) if(r.coordinator==selected_room->coordinator) area.ids.push_back(r.id);
    DisplayLock lock;
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
    render_areas();
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
void status(const std::string& text) { DisplayLock lock; lv_label_set_text(status_label,text.c_str()); }
void enqueue(Command* command) {
    if(xQueueSend(commands,&command,0)!=pdTRUE) {
        delete command;
        lv_label_set_text(status_label,"Please wait for the current action.");
    } else lv_label_set_text(status_label,"Working...");
}
lv_obj_t* label(lv_obj_t* parent,const char* text) {
    auto obj=lv_label_create(parent); lv_label_set_text(obj,text); return obj;
}
lv_obj_t* button(lv_obj_t* parent,const char* text,lv_event_cb_t callback,void* data=nullptr) {
    auto obj=lv_button_create(parent); lv_obj_set_height(obj,58);
    lv_obj_set_style_radius(obj,14,0);
    auto l=label(obj,text); lv_obj_center(l);
    lv_obj_add_event_cb(obj,callback,LV_EVENT_CLICKED,data); return obj;
}
void send_action(lv_event_t* event) {
    if(selected.id.empty()) return;
    auto command=new Command;
    command->room=selected;
    command->action=static_cast<const char*>(lv_event_get_user_data(event));
    if(command->action=="Toggle") command->action=current.playback=="PLAYING" ? "Pause" : "Play";
    if(command->action=="Mute") command->value=!current.muted;
    enqueue(command);
}
void room_changed(lv_event_t*) {
    auto index=lv_dropdown_get_selected(room_select);
    if(index<rooms.size()) {
        selected=rooms[index]; current={};
        lv_label_set_text(title_label,"Loading room..."); lv_label_set_text(artist_label,"");
        auto c=new Command; c->action="Poll"; c->room=selected; enqueue(c);
    }
}
void favorite_clicked(lv_event_t* event) {
    auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if(selected.id.empty() || index>=favorites.size()) return;
    auto c=new Command; c->action="Favorite"; c->room=selected; c->favorite=favorites[index]; enqueue(c);
}
void refresh_clicked(lv_event_t*) { auto c=new Command; c->action="Refresh"; enqueue(c); }
void volume_changed(lv_event_t* event) {
    int value=lv_slider_get_value(volume_slider);
    lv_label_set_text_fmt(volume_label,"Room volume · %d",value);
    if(lv_event_get_code(event)==LV_EVENT_RELEASED && !selected.id.empty()) {
        auto c=new Command; c->action="Volume"; c->room=selected; c->value=value; enqueue(c);
    }
}
void group_volume_changed(lv_event_t* event) {
    int value=lv_slider_get_value(group_slider);
    lv_label_set_text_fmt(group_label,"Group volume · %d",value);
    if(lv_event_get_code(event)==LV_EVENT_RELEASED && !selected.id.empty()) {
        auto c=new Command; c->action="GroupVolume"; c->room=selected; c->value=value; enqueue(c);
    }
}
void area_save_clicked(lv_event_t*) {
    if(selected.id.empty()) return;
    auto c=new Command; c->action="SaveArea"; c->room=selected; c->ssid=lv_textarea_get_text(area_name);
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN); enqueue(c);
}
void area_clicked(lv_event_t* event) {
    auto index=reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    if(index>=areas.size()) return;
    auto c=new Command; c->action="Area"; c->area_ids=areas[index].ids; enqueue(c);
}
void render_areas() {
    lv_obj_clean(area_list);
    for(size_t i=0;i<areas.size();++i) {
        auto b=button(area_list,areas[i].name.c_str(),area_clicked,reinterpret_cast<void*>(i));
        lv_obj_set_width(b,lv_pct(100));
    }
}
void group_clicked(lv_event_t* event) {
    auto index=lv_dropdown_get_selected(join_select);
    if(selected.id.empty() || index>=rooms.size()) return;
    auto c=new Command; c->action=static_cast<const char*>(lv_event_get_user_data(event));
    c->room=selected; c->destination=rooms[index]; enqueue(c);
}
void input_focus(lv_event_t* event) {
    lv_keyboard_set_textarea(keyboard,lv_event_get_target_obj(event));
    lv_obj_remove_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
}
void connect_clicked(lv_event_t*) {
    auto c=new Command; c->action="Connect";
    c->ssid=lv_textarea_get_text(ssid_input); c->password=lv_textarea_get_text(password_input);
    c->seed=lv_textarea_get_text(seed_input);
    lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN); enqueue(c);
}
lv_obj_t* input(lv_obj_t* parent,const char* placeholder,bool password=false) {
    auto obj=lv_textarea_create(parent); lv_textarea_set_one_line(obj,true);
    lv_textarea_set_password_mode(obj,password); lv_textarea_set_placeholder_text(obj,placeholder);
    lv_obj_set_width(obj,550); lv_obj_add_event_cb(obj,input_focus,LV_EVENT_FOCUSED,nullptr); return obj;
}
void column(lv_obj_t* obj) { lv_obj_set_flex_flow(obj,LV_FLEX_FLOW_COLUMN); lv_obj_set_style_pad_row(obj,18,0); }
void render_catalog(std::vector<sonos::Room> new_rooms,std::vector<sonos::Favorite> new_favorites) {
    DisplayLock lock;
    rooms=std::move(new_rooms); favorites=std::move(new_favorites);
    std::string options; uint32_t index=0;
    for(size_t i=0;i<rooms.size();++i) {
        if(i) options+='\n';
        options+=rooms[i].name;
        if(rooms[i].id==selected.id) index=i;
    }
    lv_dropdown_set_options(room_select,options.empty()?"No rooms found":options.c_str());
    lv_dropdown_set_options(join_select,options.empty()?"No rooms found":options.c_str());
    selected=rooms.empty()?sonos::Room{}:rooms[index];
    lv_dropdown_set_selected(room_select,index);
    lv_obj_clean(favorite_list);
    if(favorites.empty()) {
        auto l=label(favorite_list,"No supported favorites yet.\nAdd Apple Music or Sonos Radio favorites in the Sonos app, then Refresh.");
        lv_obj_set_width(l,lv_pct(100));
    }
    for(size_t i=0;i<favorites.size();++i) {
        auto b=button(favorite_list,(favorites[i].title+"\n"+favorites[i].provider).c_str(),favorite_clicked,reinterpret_cast<void*>(i));
        lv_obj_set_size(b,lv_pct(100),88);
        auto l=lv_obj_get_child(b,0); lv_obj_set_width(l,lv_pct(95)); lv_label_set_long_mode(l,LV_LABEL_LONG_DOT);
    }
}
void render_state(const sonos::Room& target,const sonos::State& state) {
    DisplayLock lock;
    if(target.id!=selected.id) return;
    current=state;
    lv_label_set_text(title_label,state.title.empty()?"Nothing playing":state.title.c_str());
    lv_label_set_text(artist_label,(state.artist+ (state.album.empty()?"":" · "+state.album)).c_str());
    if(!lv_obj_has_state(volume_slider,LV_STATE_PRESSED)) {
        lv_slider_set_value(volume_slider,state.volume,LV_ANIM_OFF);
        lv_label_set_text_fmt(volume_label,"Room volume · %d",state.volume);
    }
    if(!lv_obj_has_state(group_slider,LV_STATE_PRESSED)) {
        lv_slider_set_value(group_slider,state.group_volume,LV_ANIM_OFF);
        lv_label_set_text_fmt(group_label,"Group volume · %d",state.group_volume);
    }
    lv_label_set_text(lv_obj_get_child(play_button,0),state.playback=="PLAYING"?LV_SYMBOL_PAUSE:LV_SYMBOL_PLAY);
    lv_label_set_text(lv_obj_get_child(mute_button,0),state.muted?"Unmute":"Mute");
    auto supports=[&](const char* action) {
        auto values=","+state.actions+",";
        return values.find(std::string(",")+action+",")!=std::string::npos;
    };
    lv_obj_set_state(next_button,LV_STATE_DISABLED,!supports("Next"));
    lv_obj_set_state(prev_button,LV_STATE_DISABLED,!supports("Previous"));
    lv_label_set_text(status_label,network_online()?"Connected · local control":"Wi-Fi disconnected");
}
void build_ui() {
    auto display=lv_display_get_default();
    lv_display_set_theme(display,lv_theme_default_init(display,lv_palette_main(LV_PALETTE_TEAL),lv_palette_main(LV_PALETTE_GREY),true,&lv_font_montserrat_20));
    auto screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x101419),0);
    lv_obj_set_style_text_color(screen,lv_color_hex(0xf2f4f6),0);
    auto heading=label(screen,"SONOS  /  HOME"); lv_obj_align(heading,LV_ALIGN_TOP_LEFT,28,20);
    room_select=lv_dropdown_create(screen); lv_obj_set_size(room_select,390,56);
    lv_obj_align(room_select,LV_ALIGN_TOP_RIGHT,-28,10); lv_dropdown_set_options(room_select,"Choose a room");
    lv_obj_add_event_cb(room_select,room_changed,LV_EVENT_VALUE_CHANGED,nullptr);
    tabs=lv_tabview_create(screen); lv_obj_set_size(tabs,lv_pct(100),lv_pct(82)); lv_obj_align(tabs,LV_ALIGN_TOP_MID,0,80);
    lv_tabview_set_tab_bar_size(tabs,58);
    auto fav=lv_tabview_add_tab(tabs,"Favorites"); column(fav);
    auto now=lv_tabview_add_tab(tabs,"Now Playing"); column(now);
    auto group=lv_tabview_add_tab(tabs,"Rooms"); column(group);
    auto setup=lv_tabview_add_tab(tabs,"Settings"); column(setup);
    button(fav,"Refresh favorites",refresh_clicked);
    favorite_list=lv_obj_create(fav); lv_obj_set_size(favorite_list,lv_pct(100),lv_pct(78)); column(favorite_list);
    label(favorite_list,"Connect to Wi-Fi in Settings to find your Sonos system.");
    title_label=label(now,"Nothing playing"); lv_obj_set_width(title_label,lv_pct(100));
    lv_obj_set_style_text_font(title_label,&lv_font_montserrat_28,0);
    artist_label=label(now,""); lv_obj_set_width(artist_label,lv_pct(100));
    auto controls=lv_obj_create(now); lv_obj_set_size(controls,lv_pct(100),100); lv_obj_set_flex_flow(controls,LV_FLEX_FLOW_ROW);
    prev_button=button(controls,LV_SYMBOL_PREV,send_action,(void*)"Previous");
    play_button=button(controls,LV_SYMBOL_PLAY,send_action,(void*)"Toggle");
    next_button=button(controls,LV_SYMBOL_NEXT,send_action,(void*)"Next");
    button(controls,"Stop",send_action,(void*)"Stop");
    mute_button=button(controls,"Mute",send_action,(void*)"Mute");
    volume_label=label(now,"Room volume");
    volume_slider=lv_slider_create(now); lv_obj_set_size(volume_slider,lv_pct(95),28); lv_slider_set_range(volume_slider,0,100);
    lv_obj_add_event_cb(volume_slider,volume_changed,LV_EVENT_VALUE_CHANGED,nullptr);
    lv_obj_add_event_cb(volume_slider,volume_changed,LV_EVENT_RELEASED,nullptr);
    group_label=label(now,"Group volume");
    group_slider=lv_slider_create(now); lv_obj_set_size(group_slider,lv_pct(95),28); lv_slider_set_range(group_slider,0,100);
    lv_obj_add_event_cb(group_slider,group_volume_changed,LV_EVENT_VALUE_CHANGED,nullptr);
    lv_obj_add_event_cb(group_slider,group_volume_changed,LV_EVENT_RELEASED,nullptr);
    auto info=label(group,"The room selected above will join the destination below.\nJoining changes what plays in that room; the destination keeps playing."); lv_obj_set_width(info,lv_pct(100));
    join_select=lv_dropdown_create(group); lv_obj_set_width(join_select,500); lv_dropdown_set_options(join_select,"Choose destination");
    button(group,"Join selected room to destination",group_clicked,(void*)"Join");
    button(group,"Ungroup selected room",group_clicked,(void*)"Ungroup");
    button(group,"Refresh rooms",refresh_clicked);
    auto area_info=label(group,"Save the selected room's current group as an area.\nApplying an area regroups its rooms and can interrupt their current music."); lv_obj_set_width(area_info,lv_pct(100));
    area_name=input(group,"Area name, e.g. Downstairs"); lv_textarea_set_max_length(area_name,32);
    button(group,"Save current group as area",area_save_clicked);
    area_list=lv_obj_create(group); lv_obj_set_size(area_list,lv_pct(100),240); column(area_list);
    load_areas(); render_areas();
    ssid_input=input(setup,"Wi-Fi name (2.4 GHz)"); lv_textarea_set_max_length(ssid_input,32);
    password_input=input(setup,"Wi-Fi password",true); lv_textarea_set_max_length(password_input,63);
    seed_input=input(setup,"Optional Sonos speaker IP"); lv_textarea_set_max_length(seed_input,15);
    lv_textarea_set_accepted_chars(seed_input,"0123456789.");
    lv_textarea_set_text(ssid_input,setting("ssid").c_str());
    lv_textarea_set_text(password_input,setting("password").c_str());
    lv_textarea_set_text(seed_input,setting("seed").c_str());
    button(setup,"Connect & save",connect_clicked);
    label(setup,"Prototype · standalone · Apple Music + Sonos Radio favorites");
    status_label=label(screen,"Starting..."); lv_obj_set_width(status_label,lv_pct(95)); lv_obj_align(status_label,LV_ALIGN_BOTTOM_LEFT,28,-10);
    lv_label_set_long_mode(status_label,LV_LABEL_LONG_DOT);
    keyboard=lv_keyboard_create(screen); lv_obj_set_size(keyboard,lv_pct(100),lv_pct(44)); lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard,[](lv_event_t*){lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);},LV_EVENT_READY,nullptr);
    lv_obj_add_event_cb(keyboard,[](lv_event_t*){lv_obj_add_flag(keyboard,LV_OBJ_FLAG_HIDDEN);},LV_EVENT_CANCEL,nullptr);
    if(setting("ssid").empty()) lv_tabview_set_active(tabs,3,LV_ANIM_OFF);
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
    auto f=client.favorites(seed);
    render_catalog(std::move(found),std::move(f));
    catalog_loaded=true;
}
void worker(void*) {
    try {
        if(bsp_feature_enable(BSP_FEATURE_WIFI,true)!=ESP_OK) throw std::runtime_error("Cannot power the Wi-Fi module");
        vTaskDelay(pdMS_TO_TICKS(300));
        network_init(); initialized=true;
    }
    catch(const std::exception& e) { status(e.what()); ESP_LOGE(TAG,"Startup failed: %s",e.what()); }
    auto boot=new Command; boot->action="Connect"; boot->ssid=setting("ssid"); boot->password=setting("password"); boot->seed=setting("seed");
    if(!boot->ssid.empty()) { if(xQueueSend(commands,&boot,0)!=pdTRUE) delete boot; }
    else { delete boot; status(storage_ok?"Open Settings to connect to your home Wi-Fi.":"Settings storage failed; settings will not be saved. Open Settings to connect."); }
    TickType_t last_catalog_attempt=0;
    for(;;) {
        Command* raw=nullptr;
        xQueueReceive(commands,&raw,pdMS_TO_TICKS(4000));
        std::unique_ptr<Command> c(raw);
        try {
            // A boot-time connect can fail while the router is down; the Wi-Fi
            // layer keeps retrying, so load the catalog once it comes back.
            if(!c && initialized && !catalog_loaded && network_online() &&
               xTaskGetTickCount()-last_catalog_attempt>pdMS_TO_TICKS(30000)) {
                last_catalog_attempt=xTaskGetTickCount();
                refresh();
            }
            if(c) {
                if(!initialized) throw std::runtime_error("Wi-Fi hardware failed to initialize. Restart the controller.");
                if(c->action=="Connect") {
                    if(!c->seed.empty() && !sonos::valid_ipv4(c->seed)) throw std::runtime_error("Enter a valid speaker IPv4 address");
                    status("Connecting to Wi-Fi..."); network_connect(c->ssid,c->password); if(storage_ok) save_settings(*c); seed=c->seed; refresh();
                } else if(c->action=="Refresh") refresh();
                else if(c->action=="Favorite") client.play_favorite(c->room,c->favorite);
                else if(c->action=="Volume") client.volume(c->room,c->value);
                else if(c->action=="GroupVolume") client.volume(c->room,c->value,true);
                else if(c->action=="SaveArea") save_area(*c);
                else if(c->action=="Area") {
                    client.apply_area(seed,c->area_ids); refresh();
                    DisplayLock lock;
                    for(size_t i=0;i<rooms.size();++i) if(rooms[i].id==c->area_ids.front()) {selected=rooms[i]; lv_dropdown_set_selected(room_select,i); break;}
                }
                else if(c->action=="Mute") client.mute(c->room,c->value);
                else if(c->action=="Join") {client.join(c->room,c->destination); refresh();}
                else if(c->action=="Ungroup") {client.ungroup(c->room); refresh();}
                else if(c->action!="Poll") client.transport(c->room,c->action);
            }
            sonos::Room target;
            {DisplayLock lock; target=selected;}
            if(!target.id.empty()) render_state(target,client.state(target));
        } catch(const std::exception& e) {
            // Drop queued actions after any failure; never replay a possibly completed queue mutation.
            Command* pending=nullptr;
            while(xQueueReceive(commands,&pending,0)==pdTRUE) delete pending;
            status(e.what()); ESP_LOGE(TAG,"Controller operation failed: %s",e.what());
        }
    }
}
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
    await_touch_controller();
    auto display=bsp_display_start();
    if(!display) {ESP_LOGE(TAG,"Display initialization failed");return;}
    bsp_display_backlight_on();
    commands=xQueueCreate(8,sizeof(Command*));
    if(!commands) return;
    {
        DisplayLock lock;
        lv_display_set_rotation(display,LV_DISPLAY_ROTATION_90);
        build_ui();
    }
    if(xTaskCreate(worker,"sonos",24576,nullptr,4,nullptr)!=pdPASS) status("Unable to start controller task");
}
