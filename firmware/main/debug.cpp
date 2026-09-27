#include "debug.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include <cstring>
#include <string>

namespace {
const char* TAG="debug";
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
}

void debug_register(httpd_handle_t server) {
    static const httpd_uri_t uri{.uri="/screenshot",.method=HTTP_GET,.handler=screenshot,.user_ctx=nullptr};
    httpd_register_uri_handler(server,&uri);
}
