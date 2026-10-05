#include "fast_flush.hpp"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>

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
// Waits are keyed to the transaction, not the semaphore: a late interrupt can
// only wake a waiter, never satisfy a later band's wait. The band is released
// to LVGL only once its own completion has arrived, so a render buffer is
// never recycled while the PPA might still read it. A dead engine still drops
// the band after one second rather than hanging LVGL.
std::atomic<uint32_t> band_started{0}, band_finished{0}, band_dropped{0};
// The driver race documented below can strand a transaction in the engine
// queue forever; once that happens every ppa_do fails and without a fallback
// the screen freezes while the rest of the firmware keeps running. Rotating
// the band on the CPU is only a few ms and keeps the UI alive. After several
// consecutive failures the PPA path is latched off: the client cannot be
// unregistered or reset while a transaction is still pending.
lv_area_t band_area; const uint16_t* band_pixels=nullptr;
uint32_t consecutive_failures=0;
bool software_rotation=false;
bool band_on_cpu=false;  // this band was already drawn by rotate_cpu in flush()
// After the switch to CPU rotation, repaint everything once: bands from the
// wedged period may be stale or torn.
void repaint_all(void*) { lv_obj_invalidate(lv_screen_active()); lv_obj_invalidate(lv_layer_top()); }
void use_cpu_from_now_on(const char* why) {
    if(software_rotation) return;
    software_rotation=true;
    ESP_LOGE(TAG,"%s; rotating on CPU from now on",why);
    lv_async_call(repaint_all,nullptr);
}

// CPU equivalent of the SRM 90-degree rotation: logical (x,y) maps to
// framebuffer offset (1279-x)*720+y, so a column of the LVGL band lands as a
// contiguous run in the framebuffer.
void rotate_cpu(const lv_area_t* area,const uint16_t* pixels) {
    const int w=lv_area_get_width(area), h=lv_area_get_height(area);
    for(int x=0;x<w;++x) {
        uint16_t* dst=framebuffer+(NATIVE_H-1-area->x1-x)*NATIVE_W+area->y1;
        for(int y=0;y<h;++y) dst[y]=pixels[y*w+x];
    }
    // The framebuffer is in PSRAM; push the band out of the CPU cache before
    // the DPI peripheral's DMA reads it. Band offsets are almost never on a
    // 128-byte cache line, and without UNALIGNED the sync refuses and does
    // nothing: the pixels then reach the panel only on a random later
    // eviction, which showed as streaks of stale content.
    const size_t offset=(NATIVE_H-1-area->x2)*NATIVE_W+area->y1;
    if(esp_cache_msync(framebuffer+offset,w*NATIVE_W*2,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_UNALIGNED)!=ESP_OK) {
        static bool warned=false;
        if(!warned) { warned=true; ESP_LOGE(TAG,"framebuffer cache write-back failed"); }
    }
}

bool IRAM_ATTR rotated(ppa_client_handle_t,ppa_event_data_t*,void*) {
    band_finished.fetch_add(1);
    BaseType_t woken=pdFALSE;
    xSemaphoreGiveFromISR(band_done,&woken);
    return woken==pdTRUE;
}
void wait_for_band(lv_display_t* display) {
    // Drawn on the CPU already: nothing to wait for, and no PPA completion to
    // count. (Treating it as one reset the failure count every band, so the
    // switch to CPU rotation never happened and every band retried the
    // wedged engine.)
    if(band_on_cpu) { lv_display_flush_ready(display); return; }
    const uint32_t seq=band_started.load();
    // Outstanding = started - finished - dropped; dropped counts transactions
    // we timed out on, so a ghost that never completes cannot stall every
    // later wait. If its interrupt ever does arrive it just bumps finished.
    auto pending=[&]{ return band_finished.load()+band_dropped.load()<seq; };
    // ~15 ms is normal; stalls seen on hardware (OTA flash writes pausing
    // PSRAM access) finished in well under a second.
    for(int slice=0;slice<20 && pending();++slice)
        if(xSemaphoreTake(band_done,pdMS_TO_TICKS(50))!=pdTRUE) ++lost_completions;
    if(!pending()) { consecutive_failures=0; }
    // Timed out: the PPA may still be reading the band's buffer, but leaving
    // LVGL blocked costs more than a possible torn band. Render it on the CPU
    // and release, then forget the transactions we gave up on.
    else if(framebuffer && band_pixels) {
        rotate_cpu(&band_area,band_pixels);
        band_dropped.store(seq-band_finished.load());
        if(++consecutive_failures>=8) use_cpu_from_now_on("PPA transactions never complete");
    }
    lv_display_flush_ready(display);
}
void flush(lv_display_t* display,const lv_area_t* area,uint8_t* pixels) {
    if(!framebuffer) return;  // suspended: nothing to show, nothing to wait for
    const int w=lv_area_get_width(area), h=lv_area_get_height(area);
    band_area=*area; band_pixels=reinterpret_cast<const uint16_t*>(pixels);
    band_started.fetch_add(1);
    band_on_cpu=software_rotation;
    if(software_rotation) { rotate_cpu(area,band_pixels); band_finished.fetch_add(1); return; }
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
    if(ppa_do_scale_rotate_mirror(ppa,&op)!=ESP_OK) {
        // Submission failed (typically the transaction pool is empty because a
        // queued transaction never completed): draw this band on the CPU.
        rotate_cpu(area,band_pixels);
        band_finished.fetch_add(1);
        band_on_cpu=true;
        if(++consecutive_failures>=8) use_cpu_from_now_on("PPA submissions keep failing");
    }
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
void fast_flush_force_cpu() { use_cpu_from_now_on("Forced by /ppa"); }
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
    // Depth >1 matters: ppa_do_operation can lose a wakeup when a new
    // transaction is queued between the done ISR's queue check and its engine
    // semaphore release. The stranded transaction then sits at the queue head
    // until another submission flushes it; with depth 1 no submission can ever
    // be made again and the display path wedges permanently.
    ppa_client_config_t client={}; client.oper_type=PPA_OPERATION_SRM; client.max_pending_trans_num=4;
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
