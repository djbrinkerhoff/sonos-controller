#include "screen.hpp"
#include "display_power.hpp"
#include "power.hpp"
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
    // Deep stops the panel stream; only motion can wake it (on ST7121 the
    // touch controller stops scanning too). With no working motion sensor
    // there would be no way back, so the display stays touch-wakeable.
    if(next==Level::Deep && !motion_active()) next=Level::Off;
    // On USB power (docked) stay tap-wakeable: the battery is not draining.
    if(next==Level::Deep && power_external()) next=Level::Off;
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
    // Plugged in while the display sleeps: wake it to "off" so taps work again.
    if(level==Level::Deep && power_external() && !forced_at) set_level(Level::Off);
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
lv_indev_t* touch_indev=nullptr;
std::atomic<uint32_t> releases_polled{0};
// Trace of panel reads and LVGL input events, for /touch (LVGL task only).
struct TraceEntry { uint32_t ms; char kind; uint8_t state; int16_t x,y; const void* obj; };
constexpr int TRACE_LEN=96;
TraceEntry trace[TRACE_LEN]; int trace_head=0, trace_count=0;
bool polling=false;
lv_indev_read_cb_t panel_read=nullptr;
void note(char kind,uint8_t state,int16_t x,int16_t y,const void* obj) {
    trace[trace_head]={lv_tick_get(),kind,state,x,y,obj};
    trace_head=(trace_head+1)%TRACE_LEN; if(trace_count<TRACE_LEN) ++trace_count;
}
void traced_read(lv_indev_t* indev,lv_indev_data_t* data) {
    panel_read(indev,data);
    note(polling?'p':'i',data->state==LV_INDEV_STATE_PRESSED,data->point.x,data->point.y,nullptr);
}
void input_event(lv_event_t* e) {
    const auto code=lv_event_get_code(e);
    char kind=code==LV_EVENT_PRESSED?'P':code==LV_EVENT_RELEASED?'R':code==LV_EVENT_CLICKED?'C':
              code==LV_EVENT_PRESS_LOST?'L':code==LV_EVENT_SCROLL_BEGIN?'S':code==LV_EVENT_LONG_PRESSED?'G':0;
    if(kind) note(kind,0,0,0,lv_indev_get_active_obj());
}
void touch_guard(lv_timer_t*) {
    if(!touch_indev || lv_indev_get_state(touch_indev)!=LV_INDEV_STATE_PRESSED) return;
    polling=true; lv_indev_read(touch_indev); polling=false;
    if(lv_indev_get_state(touch_indev)==LV_INDEV_STATE_RELEASED) releases_polled.fetch_add(1);
}
}
void touch_guard_init(lv_indev_t* touch) {
    touch_indev=touch;
    if(!touch) return;
    panel_read=lv_indev_get_read_cb(touch);
    if(panel_read) lv_indev_set_read_cb(touch,traced_read);
    lv_indev_add_event_cb(touch,input_event,LV_EVENT_ALL,nullptr);
    if(lv_indev_get_mode(touch)==LV_INDEV_MODE_EVENT) lv_timer_create(touch_guard,15,nullptr);
}
std::string touch_trace() {
    // i/p: panel read on the interrupt / by polling (1 = pressed); P R C L S G:
    // pressed, released, clicked, press lost, scroll began, long press.
    std::string out; char line[64];
    const int start=(trace_head-trace_count+TRACE_LEN)%TRACE_LEN;
    const uint32_t t0=trace_count?trace[start].ms:0;
    for(int i=0;i<trace_count;++i) {
        const auto& t=trace[(start+i)%TRACE_LEN];
        if(t.kind=='i' || t.kind=='p') snprintf(line,sizeof line,"%7lu %c %d (%d,%d)\n",static_cast<unsigned long>(t.ms-t0),t.kind,t.state,t.x,t.y);
        else snprintf(line,sizeof line,"%7lu   %c obj %p\n",static_cast<unsigned long>(t.ms-t0),t.kind,t.obj);
        out+=line;
    }
    return out;
}
uint32_t touch_releases_polled() { return releases_polled.load(); }

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
