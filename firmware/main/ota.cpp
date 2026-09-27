#include "ota.hpp"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <atomic>
#include <algorithm>
#include <cstring>

namespace {
const char* TAG="ota";
std::atomic<bool> busy{false};
esp_timer_handle_t restart_timer;

void restart_now(void*) { esp_restart(); }

esp_err_t answer(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"text/plain");
    httpd_resp_send(req,text,HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t upload(httpd_req_t* req) {
    if(busy.exchange(true)) return answer(req,"409 Conflict","OTA already in progress");
    struct Guard { bool armed=true; ~Guard(){if(armed) busy=false;} } guard;

    const esp_partition_t* part=esp_ota_get_next_update_partition(nullptr);
    if(!part) return answer(req,"500 Internal Server Error","no OTA partition configured");
    if(!req->content_len) return answer(req,"400 Bad Request","empty body");
    if(req->content_len>part->size) return answer(req,"413 Content Too Large","image exceeds the OTA partition");

    esp_ota_handle_t ota;
    if(auto err=esp_ota_begin(part,req->content_len,&ota); err!=ESP_OK)
        return answer(req,"500 Internal Server Error",esp_err_to_name(err));
    struct Abort { esp_ota_handle_t h; bool armed=true; ~Abort(){if(armed) esp_ota_abort(h);} } abort{ota};

    char scratch[2048];
    size_t remaining=req->content_len;
    while(remaining) {
        int n=httpd_req_recv(req,scratch,std::min(remaining,sizeof(scratch)));
        if(n<=0) return answer(req,"400 Bad Request","upload interrupted");
        if(auto err=esp_ota_write(ota,scratch,n); err!=ESP_OK)
            return answer(req,"500 Internal Server Error",esp_err_to_name(err));
        remaining-=n;
    }
    // esp_ota_end validates the appended secure-boot signature when
    // CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT is set.
    if(auto err=esp_ota_end(ota); err!=ESP_OK) {
        ESP_LOGW(TAG,"image rejected: %s",esp_err_to_name(err));
        return answer(req,"400 Bad Request",esp_err_to_name(err));
    }
    abort.armed=false;
    if(auto err=esp_ota_set_boot_partition(part); err!=ESP_OK)
        return answer(req,"500 Internal Server Error",esp_err_to_name(err));

    ESP_LOGI(TAG,"wrote %u bytes to %s; rebooting",static_cast<unsigned>(req->content_len),part->label);
    guard.armed=false; // keep rejecting uploads until the reboot lands
    answer(req,"200 OK","OTA complete, rebooting");
    esp_timer_start_once(restart_timer,1000000);
    return ESP_OK;
}

const char* state_name(esp_ota_img_states_t state) {
    switch(state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "unknown";
    }
}
}

void ota_register(httpd_handle_t server) {
    if(!server) return;
    esp_timer_create_args_t args{};
    args.callback=restart_now; args.name="ota_restart";
    esp_timer_create(&args,&restart_timer);
    httpd_uri_t uri{};
    uri.uri="/ota"; uri.method=HTTP_POST; uri.handler=upload;
    httpd_register_uri_handler(server,&uri);
}

void ota_log_boot() {
    const esp_partition_t* running=esp_ota_get_running_partition();
    const esp_app_desc_t* desc=esp_app_get_description();
    if(!running || !desc) return;
    esp_ota_img_states_t state;
    const char* name=esp_ota_get_state_partition(running,&state)==ESP_OK?state_name(state):"unknown";
    ESP_LOGI(TAG,"running %s @ 0x%lx, state %s, version %s (%s %s)",
             running->label,running->address,name,desc->version,desc->date,desc->time);
}

void ota_mark_healthy() {
    const esp_partition_t* running=esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if(!running || esp_ota_get_state_partition(running,&state)!=ESP_OK) return;
    if(state!=ESP_OTA_IMG_PENDING_VERIFY) return;
    if(auto err=esp_ota_mark_app_valid_cancel_rollback(); err==ESP_OK)
        ESP_LOGI(TAG,"app marked valid; rollback cancelled");
    else
        ESP_LOGW(TAG,"mark valid failed: %s",esp_err_to_name(err));
}
