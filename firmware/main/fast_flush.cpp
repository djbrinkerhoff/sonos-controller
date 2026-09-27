#include "fast_flush.hpp"
#include "driver/ppa.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {
const char* TAG="fast_flush";
// The port's flush does a blocking PPA rotation into its own buffer and then a
// DMA2D copy into the framebuffer: the CPU waits on both for every band (about
// 14 ms per full frame). Here the PPA writes the rotated band directly into the
// framebuffer and reports completion from its interrupt, so LVGL renders the
// next band (double-buffered) while the PPA works, and the copy disappears.
constexpr int NATIVE_W=720, NATIVE_H=1280;  // the panel is portrait; the UI is rotated 90 degrees
ppa_client_handle_t ppa=nullptr;
uint16_t* framebuffer=nullptr;
// LVGL would busy-wait for each band; on hardware that spin starved CPU 0
// (task watchdog) when a PPA completion never arrived during an OTA flash
// write. Waiting on a semaphore yields the core, and a timeout guarantees
// progress: a lost completion costs one stale band, never a hang.
SemaphoreHandle_t band_done=nullptr;
volatile uint32_t lost_completions=0;

bool IRAM_ATTR rotated(ppa_client_handle_t,ppa_event_data_t*,void*) {
    BaseType_t woken=pdFALSE;
    xSemaphoreGiveFromISR(band_done,&woken);
    return woken==pdTRUE;
}
void wait_for_band(lv_display_t* display) {
    if(xSemaphoreTake(band_done,pdMS_TO_TICKS(50))!=pdTRUE) ++lost_completions;
    lv_display_flush_ready(display);
}
void flush(lv_display_t* display,const lv_area_t* area,uint8_t* pixels) {
    if(!framebuffer) { xSemaphoreGive(band_done); return; }  // suspended: nothing to show
    const int w=lv_area_get_width(area), h=lv_area_get_height(area);
    // Same mapping as esp_lvgl_port's PPA path for LV_DISPLAY_ROTATION_90.
    ppa_srm_oper_config_t op={};
    op.in.buffer=pixels; op.in.pic_w=w; op.in.pic_h=h; op.in.block_w=w; op.in.block_h=h;
    op.in.srm_cm=PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer=framebuffer; op.out.buffer_size=NATIVE_W*NATIVE_H*2;
    op.out.pic_w=NATIVE_W; op.out.pic_h=NATIVE_H;
    op.out.block_offset_x=area->y1; op.out.block_offset_y=lv_display_get_horizontal_resolution(display)-area->x2-1;
    op.out.srm_cm=PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle=static_cast<ppa_srm_rotation_angle_t>(LV_DISPLAY_ROTATION_90);
    op.scale_x=1.0f; op.scale_y=1.0f;
    op.mode=PPA_TRANS_MODE_NON_BLOCKING; op.user_data=display;
    if(ppa_do_scale_rotate_mirror(ppa,&op)!=ESP_OK) xSemaphoreGive(band_done);  // nothing to wait for
}
// The first fields of esp_lvgl_port 2.6's private per-display context. The
// result is verified below by asking the panel for its framebuffer.
struct PortContextPrefix { int type; esp_lcd_panel_io_handle_t io; esp_lcd_panel_handle_t panel; };
PortContextPrefix* context=nullptr;
}
static bool installed=false;
esp_lcd_panel_handle_t fast_flush_panel() { return installed?context->panel:nullptr; }
esp_lcd_panel_io_handle_t fast_flush_panel_io() { return installed?context->io:nullptr; }
void fast_flush_suspend() { framebuffer=nullptr; }
bool fast_flush_resume(esp_lcd_panel_handle_t panel) {
    void* fb=nullptr;
    if(!installed || esp_lcd_dpi_panel_get_frame_buffer(panel,1,&fb)!=ESP_OK || !fb) return false;
    context->panel=panel;  // keep the port's copy valid for anything that uses it
    framebuffer=static_cast<uint16_t*>(fb);
    return true;
}

const uint16_t* fast_flush_framebuffer() { return framebuffer; }
uint32_t fast_flush_lost_completions() { return lost_completions; }
bool fast_flush_install(lv_display_t* display) {
    if(lv_display_get_rotation(display)!=LV_DISPLAY_ROTATION_90) return false;
    context=static_cast<PortContextPrefix*>(lv_display_get_driver_data(display));
    void* fb=nullptr;
    if(!context || !context->panel || esp_lcd_dpi_panel_get_frame_buffer(context->panel,1,&fb)!=ESP_OK || !fb) {
        framebuffer=nullptr;
        ESP_LOGW(TAG,"panel framebuffer unavailable; keeping the port's flush");
        return false;
    }
    band_done=xSemaphoreCreateBinary();
    if(!band_done) return false;
    ppa_client_config_t client={}; client.oper_type=PPA_OPERATION_SRM; client.max_pending_trans_num=1;
    ppa_event_callbacks_t callbacks={}; callbacks.on_trans_done=rotated;
    if(ppa_register_client(&client,&ppa)!=ESP_OK || ppa_client_register_event_callbacks(ppa,&callbacks)!=ESP_OK) {
        ESP_LOGW(TAG,"PPA client unavailable; keeping the port's flush");
        return false;
    }
    framebuffer=static_cast<uint16_t*>(fb);
    installed=true;
    lv_display_set_flush_cb(display,flush);
    lv_display_set_flush_wait_cb(display,wait_for_band);
    ESP_LOGI(TAG,"PPA rotates directly into the framebuffer");
    return true;
}
