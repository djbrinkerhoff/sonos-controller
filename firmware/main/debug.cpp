#include "debug.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "esp_heap_caps.h"
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
}

void debug_register(httpd_handle_t server) {
    static const httpd_uri_t shot{.uri="/screenshot",.method=HTTP_GET,.handler=screenshot,.user_ctx=nullptr};
    static const httpd_uri_t list{.uri="/tasks",.method=HTTP_GET,.handler=tasks,.user_ctx=nullptr};
    static const httpd_uri_t log{.uri="/log",.method=HTTP_GET,.handler=log_dump,.user_ctx=nullptr};
    httpd_register_uri_handler(server,&shot);
    httpd_register_uri_handler(server,&list);
    httpd_register_uri_handler(server,&log);
}
void debug_log_init() {
    log_ring=static_cast<char*>(heap_caps_malloc(LOG_SIZE,MALLOC_CAP_SPIRAM));
    if(log_ring) console=esp_log_set_vprintf(log_hook);
}
