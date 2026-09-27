#include "screen.hpp"
#include "display_power.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <atomic>

namespace {
const char* TAG="screen";
// Off keeps the display stream running so a tap still wakes it (the touch
// controller scans only while the panel is driven). Deep stops the stream:
// about 17 mA less, but only motion wakes it.
enum class Level { Awake, Dim, Off, Deep };
std::atomic<Level> level{Level::Awake};
std::atomic<int64_t> last_motion{0};
void (*wake_callback)()=nullptr;
// While the backlight is off, a full-screen shield takes the waking touch so
// it cannot press a control the user could not see.
lv_obj_t* shield=nullptr;

bool dark(Level l) { return l==Level::Off || l==Level::Deep; }
void set_level(Level next) {
    const Level was=level.exchange(next);
    if(was==next) return;
    const int percent=next==Level::Awake?100:next==Level::Dim?CONFIG_TAB5_DIM_PERCENT:0;
    if(was==Level::Deep) display_wake();   // redrawn before the backlight shows it
    bsp_display_brightness_set(percent);
    if(next==Level::Deep) display_sleep();
    ESP_LOGI(TAG,"%s",next==Level::Awake?"awake":next==Level::Dim?"dimmed":next==Level::Off?"off":"off, display asleep");
    if(dark(next)) lv_obj_remove_flag(shield,LV_OBJ_FLAG_HIDDEN);
    if(dark(was) && !dark(next)) {
        // A motion wake has no finger down to lift, so drop the shield now.
        if(!lv_obj_has_state(shield,LV_STATE_PRESSED)) lv_obj_add_flag(shield,LV_OBJ_FLAG_HIDDEN);
        if(wake_callback) wake_callback();
    }
}
int64_t forced_at=0;  // debug: esp_timer time of a forced screen-off
void tick(lv_timer_t*) {
    int64_t idle_ms=lv_display_get_inactive_time(nullptr);
    if(auto moved=last_motion.load()) idle_ms=std::min<int64_t>(idle_ms,(esp_timer_get_time()-moved)/1000);
    if(forced_at) {
        // Stay off until a touch or motion newer than the forced off.
        if(idle_ms>=(esp_timer_get_time()-forced_at)/1000) return;
        forced_at=0;
    }
    if(idle_ms<CONFIG_TAB5_DIM_SECONDS*1000LL) set_level(Level::Awake);
    else if(idle_ms<CONFIG_TAB5_OFF_SECONDS*1000LL) { if(level==Level::Awake) set_level(Level::Dim); }
    else if(idle_ms<(CONFIG_TAB5_OFF_SECONDS+CONFIG_TAB5_DEEP_OFF_MINUTES*60)*1000LL) { if(level!=Level::Deep) set_level(Level::Off); }
    else set_level(Level::Deep);
}
void shield_released(lv_event_t*) { lv_obj_add_flag(shield,LV_OBJ_FLAG_HIDDEN); }
}

void screen_init(void (*on_wake)()) {
    wake_callback=on_wake;
    shield=lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(shield);
    lv_obj_set_size(shield,lv_pct(100),lv_pct(100));
    lv_obj_add_flag(shield,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(shield,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(shield,shield_released,LV_EVENT_RELEASED,nullptr);
    lv_obj_add_event_cb(shield,shield_released,LV_EVENT_PRESS_LOST,nullptr);
    lv_timer_create(tick,250,nullptr);
}
void screen_force_off(bool deep) { set_level(deep?Level::Deep:Level::Off); forced_at=esp_timer_get_time(); }
void screen_note_motion() { last_motion=esp_timer_get_time(); }
bool screen_off() { return dark(level); }
