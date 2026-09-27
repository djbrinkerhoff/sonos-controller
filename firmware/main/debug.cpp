#include "debug.hpp"
#include "ui.hpp"
#include "fast_flush.hpp"
#include "esp_cache.h"
#include "misc/lv_profiler_builtin_private.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <vector>
#include <string>

namespace {
const char* TAG="debug";
constexpr size_t LOG_SIZE=32*1024;
char* log_ring=nullptr;
size_t log_head=0, log_used=0;
portMUX_TYPE log_mux=portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t console=nullptr;
int log_hook(const char* format,va_list args) {
    char line[256];
    va_list copy; va_copy(copy,args);
    int n=vsnprintf(line,sizeof(line),format,copy);
    va_end(copy);
    if(n>0 && log_ring) {
        size_t len=std::min<size_t>(n,sizeof(line)-1);
        portENTER_CRITICAL(&log_mux);
        for(size_t i=0;i<len;++i) { log_ring[log_head]=line[i]; log_head=(log_head+1)%LOG_SIZE; }
        log_used=std::min(log_used+len,LOG_SIZE);
        portEXIT_CRITICAL(&log_mux);
    }
    return console?console(format,args):n;
}
esp_err_t log_dump(httpd_req_t* request) {
    std::string text(LOG_SIZE,'\0');
    portENTER_CRITICAL(&log_mux);
    const size_t used=log_used, start=(log_head+LOG_SIZE-used)%LOG_SIZE;
    for(size_t i=0;i<used;++i) text[i]=log_ring[(start+i)%LOG_SIZE];
    portEXIT_CRITICAL(&log_mux);
    text.resize(used);
    httpd_resp_set_type(request,"text/plain");
    return httpd_resp_send(request,text.data(),text.size());
}
void put16(uint8_t* p,uint16_t v) { p[0]=v; p[1]=v>>8; }
void put32(uint8_t* p,uint32_t v) { for(int i=0;i<4;++i) p[i]=v>>(8*i); }
esp_err_t screenshot(httpd_req_t* request) {
    lv_draw_buf_t* shot=nullptr;
    bsp_display_lock(0);
    // LVGL RGB888 is stored B,G,R: already BMP's byte order.
    shot=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB888);
    bsp_display_unlock();
    if(!shot) return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"snapshot failed");
    const uint32_t w=shot->header.w, h=shot->header.h, row=(w*3+3)&~3u;
    uint8_t header[54]{};
    header[0]='B'; header[1]='M'; put32(header+2,54+row*h); put32(header+10,54);
    put32(header+14,40); put32(header+18,w); put32(header+22,h); put16(header+26,1); put16(header+28,24);
    put32(header+34,row*h);
    httpd_resp_set_type(request,"image/bmp");
    esp_err_t result=httpd_resp_send_chunk(request,reinterpret_cast<const char*>(header),sizeof(header));
    std::string line(row,'\0');
    for(int32_t y=h-1;y>=0 && result==ESP_OK;--y) { // BMP rows run bottom-up
        std::memcpy(line.data(),shot->data+y*shot->header.stride,w*3);
        result=httpd_resp_send_chunk(request,line.data(),row);
    }
    lv_draw_buf_destroy(shot);
    if(result==ESP_OK) result=httpd_resp_send_chunk(request,nullptr,0);
    ESP_LOGI(TAG,"screenshot %ux%u -> %s",static_cast<unsigned>(w),static_cast<unsigned>(h),esp_err_to_name(result));
    return result;
}

// What the panel actually shows: the DPI framebuffer, rotated back to landscape.
// /screenshot renders LVGL's widgets instead, so it cannot catch flush bugs.
esp_err_t panel(httpd_req_t* request) {
    const uint16_t* fb=fast_flush_framebuffer();
    if(!fb) return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"framebuffer unavailable");
    esp_cache_msync(const_cast<uint16_t*>(fb),720*1280*2,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const uint32_t w=1280, h=720, row=w*3;
    uint8_t header[54]{};
    header[0]='B'; header[1]='M'; put32(header+2,54+row*h); put32(header+10,54);
    put32(header+14,40); put32(header+18,w); put32(header+22,h); put16(header+26,1); put16(header+28,24); put32(header+34,row*h);
    httpd_resp_set_type(request,"image/bmp");
    esp_err_t result=httpd_resp_send_chunk(request,reinterpret_cast<const char*>(header),sizeof(header));
    std::string line(row,'\0');
    for(int32_t y=h-1;y>=0 && result==ESP_OK;--y) {
        for(uint32_t x=0;x<w;++x) {
            const uint16_t p=fb[(1279-x)*720+y];  // logical (x,y) -> native (y, 1279-x)
            line[x*3]=(p&31)<<3; line[x*3+1]=((p>>5)&63)<<2; line[x*3+2]=(p>>11)<<3;
        }
        result=httpd_resp_send_chunk(request,line.data(),row);
    }
    if(result==ESP_OK) result=httpd_resp_send_chunk(request,nullptr,0);
    return result;
}

// Task states, stack headroom (words) and CPU share since boot.
esp_err_t tasks(httpd_req_t* request) {
    std::string text(4096,'\0');
    vTaskList(text.data());
    std::string stats(4096,'\0');
    vTaskGetRunTimeStats(stats.data());
    text=std::string(text.c_str())+"\n"+stats.c_str();
    httpd_resp_set_type(request,"text/plain");
    return httpd_resp_send(request,text.data(),text.size());
}

// ---- rendering performance ----------------------------------------------------
// Each refresh cycle that actually flushed pixels: how long it took and how
// many bands it was drawn in. Taps injected over HTTP are timestamped so a
// tap can be followed to the frames it caused.
struct Frame { int64_t end_us; uint32_t render_us, flush_us; uint16_t flushes; };
constexpr size_t FRAMES=128;
Frame frames[FRAMES]; size_t frame_head=0, frame_count=0;
int64_t refresh_start=0, flush_start=0, flush_total=0; uint16_t flush_count=0;
lv_area_t last_band{};
int64_t tap_down_us=0, tap_up_us=0;
void display_event(lv_event_t* e) {
    switch(lv_event_get_code(e)) {
        case LV_EVENT_REFR_START: refresh_start=esp_timer_get_time(); flush_count=0; flush_total=0; break;
        case LV_EVENT_FLUSH_START:
            ++flush_count; flush_start=esp_timer_get_time();
            if(auto a=static_cast<lv_area_t*>(lv_event_get_param(e))) last_band=*a;
            break;
        case LV_EVENT_FLUSH_FINISH: flush_total+=esp_timer_get_time()-flush_start; break;
        case LV_EVENT_REFR_READY:
            if(flush_count) {
                const int64_t now=esp_timer_get_time();
                frames[frame_head]={now,static_cast<uint32_t>(now-refresh_start),static_cast<uint32_t>(flush_total),flush_count};
                frame_head=(frame_head+1)%FRAMES; frame_count=std::min(frame_count+1,FRAMES);
            }
            break;
        default: break;
    }
}
// A second pointer device that /tap drives, for measuring tap-to-frame time.
// The press lasts a number of LVGL reads rather than a fixed time, so a busy
// or sleeping LVGL task cannot miss it; times are taken from the first read.
struct { bool pressed=false; int32_t raw_x=0, raw_y=0; int reads_left=0; int32_t step_raw_x=0; } virtual_touch;
void virtual_read(lv_indev_t*,lv_indev_data_t* data) {
    if(virtual_touch.pressed) {
        if(!tap_down_us) tap_down_us=esp_timer_get_time();
        else virtual_touch.raw_x+=virtual_touch.step_raw_x;  // a drag moves every read
        if(--virtual_touch.reads_left<0) { virtual_touch.pressed=false; tap_up_us=esp_timer_get_time(); }
    }
    data->point.x=virtual_touch.raw_x; data->point.y=virtual_touch.raw_y;
    data->state=virtual_touch.pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
esp_err_t tap(httpd_req_t* request) {
    char query[48]{}, x[8]{}, y[8]{};
    if(httpd_req_get_url_query_str(request,query,sizeof query)!=ESP_OK || httpd_query_key_value(query,"x",x,sizeof x)!=ESP_OK ||
       httpd_query_key_value(query,"y",y,sizeof y)!=ESP_OK)
        return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"use /tap?x=..&y=.. in screen coordinates");
    bsp_display_lock(0);
    // Logical landscape (x,y) back to the panel's native portrait coordinates.
    virtual_touch.raw_x=atoi(y); virtual_touch.raw_y=1279-atoi(x);
    virtual_touch.pressed=true; virtual_touch.reads_left=3; virtual_touch.step_raw_x=0; tap_down_us=0; tap_up_us=0;
    bsp_display_unlock();
    return httpd_resp_sendstr(request,"ok");
}
esp_err_t perf(httpd_req_t* request) {
    std::string out; char line[96];
    bsp_display_lock(0);
    const size_t n=frame_count;
    std::vector<Frame> copy(n);
    for(size_t i=0;i<n;++i) copy[i]=frames[(frame_head+FRAMES-n+i)%FRAMES];
    const int64_t down=tap_down_us, up=tap_up_us;
    const lv_area_t band=last_band;
    auto* buf=lv_display_get_buf_active(lv_display_get_default());
    const uint32_t buf_bytes=buf?buf->data_size:0;
    const size_t dma_free=heap_caps_get_free_size(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    const size_t dma_block=heap_caps_get_largest_free_block(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    bsp_display_unlock();
    uint32_t worst=0; uint64_t total=0;
    for(auto& f:copy) { worst=std::max(worst,f.render_us); total+=f.render_us; }
    snprintf(line,sizeof line,"frames %u  avg %.1f ms  max %.1f ms\n",static_cast<unsigned>(n),n?total/1000.0/n:0.0,worst/1000.0); out+=line;
    snprintf(line,sizeof line,"draw buffer %u bytes, last band %dx%d at y=%d\n",static_cast<unsigned>(buf_bytes),
             static_cast<int>(lv_area_get_width(&band)),static_cast<int>(lv_area_get_height(&band)),static_cast<int>(band.y1)); out+=line;
    snprintf(line,sizeof line,"internal DMA heap free %u KB, largest block %u KB, lost PPA completions %u\n",static_cast<unsigned>(dma_free/1024),
             static_cast<unsigned>(dma_block/1024),static_cast<unsigned>(fast_flush_lost_completions())); out+=line;
    if(down) { snprintf(line,sizeof line,"last tap: down, up at +%.0f ms\n",(up-down)/1000.0); out+=line; }
    for(auto& f:copy) {
        const double since=down?(f.end_us-down)/1000.0:0;
        if(down && f.end_us<down) continue;  // only frames since the last tap
        snprintf(line,sizeof line,"  +%7.0f ms  frame %6.1f ms  %2u bands  flush %5.1f ms\n",since,f.render_us/1000.0,f.flushes,f.flush_us/1000.0); out+=line;
    }
    httpd_resp_set_type(request,"text/plain");
    return httpd_resp_send(request,out.data(),out.size());
}

// Drags vertically (screen coordinates) over a number of input reads, to
// measure scrolling frame rate: /drag?x=700&y=600&dy=-400&reads=20
esp_err_t drag(httpd_req_t* request) {
    char query[64]{}, x[8]{}, y[8]{}, dy[8]{}, reads[8]{};
    if(httpd_req_get_url_query_str(request,query,sizeof query)!=ESP_OK || httpd_query_key_value(query,"x",x,sizeof x)!=ESP_OK ||
       httpd_query_key_value(query,"y",y,sizeof y)!=ESP_OK || httpd_query_key_value(query,"dy",dy,sizeof dy)!=ESP_OK)
        return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"use /drag?x=..&y=..&dy=..[&reads=..]");
    const int n=httpd_query_key_value(query,"reads",reads,sizeof reads)==ESP_OK?std::max(2,atoi(reads)):20;
    bsp_display_lock(0);
    virtual_touch.raw_x=atoi(y); virtual_touch.raw_y=1279-atoi(x);  // logical y maps to raw x
    virtual_touch.step_raw_x=atoi(dy)/n;
    virtual_touch.pressed=true; virtual_touch.reads_left=n; tap_down_us=0; tap_up_us=0;
    bsp_display_unlock();
    return httpd_resp_sendstr(request,"ok");
}

#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
// LVGL's built-in profiler with microsecond ticks: /profile?on=1 starts a
// capture, /profile stops it and returns the trace (ftrace text format).
std::string* profile_out=nullptr;
uint64_t profile_tick() { return esp_timer_get_time(); }
void profile_flush(const char* text) { if(profile_out) *profile_out+=text; }
esp_err_t profile(httpd_req_t* request) {
    char query[16]{}, on[4]{};
    const bool start=httpd_req_get_url_query_str(request,query,sizeof query)==ESP_OK && httpd_query_key_value(query,"on",on,sizeof on)==ESP_OK;
    std::string out;
    bsp_display_lock(0);
    static bool configured=false;
    if(!configured) {
        lv_profiler_builtin_uninit();
        lv_profiler_builtin_config_t config; lv_profiler_builtin_config_init(&config);
        config.tick_per_sec=1000000; config.tick_get_cb=profile_tick; config.flush_cb=profile_flush;
        lv_profiler_builtin_init(&config); configured=true;
    }
    if(start) lv_profiler_builtin_set_enable(true);
    else { lv_profiler_builtin_set_enable(false); profile_out=&out; lv_profiler_builtin_flush(); profile_out=nullptr; }
    bsp_display_unlock();
    httpd_resp_set_type(request,"text/plain");
    return start?httpd_resp_sendstr(request,"profiling"):httpd_resp_send(request,out.data(),out.size());
}
#endif

// Copy throughput from PSRAM into internal RAM, as the renderer does for images:
// aligned and 2-byte-misaligned rows, with LVGL's lv_memcpy and the C library.
esp_err_t membench(httpd_req_t* request) {
    constexpr size_t ROW=392, ROWS=196, SIZE=ROW*ROWS;
    auto* src=static_cast<uint8_t*>(heap_caps_malloc(SIZE+8,MALLOC_CAP_SPIRAM));
    auto* dst=static_cast<uint8_t*>(heap_caps_malloc(ROW+8,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    if(!src || !dst) { heap_caps_free(src); heap_caps_free(dst); return httpd_resp_send_err(request,HTTPD_500_INTERNAL_SERVER_ERROR,"no memory"); }
    std::string out; char line[96];
    for(int variant=0;variant<4;++variant) {
        const bool misaligned=variant&1, clib=variant&2;
        const size_t off=misaligned?2:0;
        const int64_t start=esp_timer_get_time();
        for(int pass=0;pass<10;++pass)
            for(size_t r=0;r<ROWS;++r) {
                if(clib) memcpy(dst+off,src+r*ROW,ROW); else lv_memcpy(dst+off,src+r*ROW,ROW);
            }
        const double us=esp_timer_get_time()-start;
        snprintf(line,sizeof line,"%-8s %-10s %6.1f MB/s\n",clib?"libc":"lv",misaligned?"misaligned":"aligned",10.0*SIZE/us);
        out+=line;
    }
    heap_caps_free(src); heap_caps_free(dst);
    httpd_resp_set_type(request,"text/plain");
    return httpd_resp_send(request,out.data(),out.size());
}

// Switches the visible view so every screen can be captured remotely.
esp_err_t show(httpd_req_t* request) {
    char query[32]{}, value[8]{};
    if(httpd_req_get_url_query_str(request,query,sizeof query)!=ESP_OK || httpd_query_key_value(query,"view",value,sizeof value)!=ESP_OK)
        return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"use /ui?view=0..4");
    const int n=atoi(value);
    if(n<0 || n>4) return httpd_resp_send_err(request,HTTPD_400_BAD_REQUEST,"view must be 0..4");
    ui_show(static_cast<View>(n));
    return httpd_resp_sendstr(request,"ok");
}
}

void debug_register(httpd_handle_t server) {
    static const httpd_uri_t shot{.uri="/screenshot",.method=HTTP_GET,.handler=screenshot,.user_ctx=nullptr};
    static const httpd_uri_t list{.uri="/tasks",.method=HTTP_GET,.handler=tasks,.user_ctx=nullptr};
    static const httpd_uri_t log{.uri="/log",.method=HTTP_GET,.handler=log_dump,.user_ctx=nullptr};
    static const httpd_uri_t ui{.uri="/ui",.method=HTTP_GET,.handler=show,.user_ctx=nullptr};
    static const httpd_uri_t perf_uri{.uri="/perf",.method=HTTP_GET,.handler=perf,.user_ctx=nullptr};
    static const httpd_uri_t tap_uri{.uri="/tap",.method=HTTP_GET,.handler=tap,.user_ctx=nullptr};
    static const httpd_uri_t drag_uri{.uri="/drag",.method=HTTP_GET,.handler=drag,.user_ctx=nullptr};
    static const httpd_uri_t panel_uri{.uri="/panel",.method=HTTP_GET,.handler=panel,.user_ctx=nullptr};
    httpd_register_uri_handler(server,&panel_uri);
    httpd_register_uri_handler(server,&drag_uri);
    static const httpd_uri_t membench_uri{.uri="/membench",.method=HTTP_GET,.handler=membench,.user_ctx=nullptr};
    httpd_register_uri_handler(server,&membench_uri);
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
    static const httpd_uri_t profile_uri{.uri="/profile",.method=HTTP_GET,.handler=profile,.user_ctx=nullptr};
    httpd_register_uri_handler(server,&profile_uri);
#endif
    httpd_register_uri_handler(server,&ui);
    httpd_register_uri_handler(server,&perf_uri);
    httpd_register_uri_handler(server,&tap_uri);
    bsp_display_lock(0);
    auto display=lv_display_get_default();
    for(auto code:{LV_EVENT_REFR_START,LV_EVENT_FLUSH_START,LV_EVENT_FLUSH_FINISH,LV_EVENT_REFR_READY}) lv_display_add_event_cb(display,display_event,code,nullptr);
    auto pointer=lv_indev_create(); lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,virtual_read); lv_indev_set_display(pointer,display);
    bsp_display_unlock();
    httpd_register_uri_handler(server,&shot);
    httpd_register_uri_handler(server,&list);
    httpd_register_uri_handler(server,&log);
}
void debug_log_init() {
    log_ring=static_cast<char*>(heap_caps_malloc(LOG_SIZE,MALLOC_CAP_SPIRAM));
    if(log_ring) console=esp_log_set_vprintf(log_hook);
}
