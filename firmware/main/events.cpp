#include "events.hpp"
#include "esp_http_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <mutex>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace {
const char* TAG="events";
constexpr EventBits_t WAKE = 1;
constexpr int PORT = 3400;
constexpr int REQUESTED_TIMEOUT_S = 600;
constexpr size_t MAX_NOTIFY_BODY = 64*1024;

struct Subscription {
    EventTarget target;
    std::string sid;
    int64_t renew_at = 0;   // esp_timer time to renew at (half the granted timeout)
};

EventGroupHandle_t bits;
std::mutex lock;
httpd_handle_t server;
void (*callback)(const char*) = nullptr;
std::vector<EventTarget> targets;          // desired set, written by events_set_targets
std::vector<Subscription> subscriptions;   // active set, owned by the maintenance task
std::string bound_ip;                      // our address the subscriptions were made for

std::string sta_ip() {
    esp_netif_t* netif=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    char text[16]{};
    if(netif && esp_netif_get_ip_info(netif,&info)==ESP_OK && info.ip.addr)
        esp_ip4addr_ntoa(&info.ip,text,sizeof(text));
    return text;
}

std::string event_path(const std::string& service) {
    if(service=="ZoneGroupTopology") return "/ZoneGroupTopology/Event";
    return "/MediaRenderer/"+service+"/Event";
}

// esp_http_client_get_header() returns *request* headers; response headers
// only arrive through HTTP_EVENT_ON_HEADER.
struct ResponseHeaders { std::string sid, timeout; };
esp_err_t on_header(esp_http_client_event_t* e) {
    if(e->event_id==HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        auto* h=static_cast<ResponseHeaders*>(e->user_data);
        if(!strcasecmp(e->header_key,"SID")) h->sid=e->header_value;
        else if(!strcasecmp(e->header_key,"TIMEOUT")) h->timeout=e->header_value;
    }
    return ESP_OK;
}

// One SUBSCRIBE/UNSUBSCRIBE round-trip. Returns the response headers of a 200.
esp_err_t gena(esp_http_client_method_t method, const Subscription& sub,
               const std::string& callback_uri, std::string* sid, int* granted) {
    auto url="http://"+sub.target.ip+":1400"+event_path(sub.target.service);
    esp_http_client_config_t config{};
    config.url=url.c_str(); config.timeout_ms=5000;
    config.disable_auto_redirect=true;
    ResponseHeaders headers;
    config.event_handler=on_header; config.user_data=&headers;
    auto client=esp_http_client_init(&config);
    if(!client) return ESP_FAIL;
    struct Cleanup { esp_http_client_handle_t h; ~Cleanup(){esp_http_client_cleanup(h);} } cleanup{client};
    esp_http_client_set_method(client,method);
    if(!callback_uri.empty()) {
        auto value="<"+callback_uri+">";
        esp_http_client_set_header(client,"CALLBACK",value.c_str());
        esp_http_client_set_header(client,"NT","upnp:event");
    }
    if(!sub.sid.empty()) esp_http_client_set_header(client,"SID",sub.sid.c_str());
    if(method==HTTP_METHOD_SUBSCRIBE) {
        auto timeout="Second-"+std::to_string(REQUESTED_TIMEOUT_S);
        esp_http_client_set_header(client,"TIMEOUT",timeout.c_str());
    }
    // Sonos answers with headers only, no Content-Length, and closes; reading
    // a body makes esp_http_client report incomplete data. The headers carry
    // everything needed.
    auto result=esp_http_client_open(client,0);
    if(result==ESP_OK && esp_http_client_fetch_headers(client)<0) result=ESP_FAIL;
    if(result!=ESP_OK || esp_http_client_get_status_code(client)!=200) {
        ESP_LOGW(TAG,"%s %s on %s failed: %s (HTTP %d)",method==HTTP_METHOD_SUBSCRIBE?"subscribe":"unsubscribe",
                 sub.target.service.c_str(),sub.target.ip.c_str(),esp_err_to_name(result),
                 esp_http_client_get_status_code(client));
        return result!=ESP_OK?result:ESP_FAIL;
    }
    if(sid) {
        if(headers.sid.empty()) {
            ESP_LOGW(TAG,"subscribe %s on %s: no SID in response",sub.target.service.c_str(),sub.target.ip.c_str());
            return ESP_FAIL;
        }
        *sid=headers.sid;
    }
    if(granted) {
        *granted=REQUESTED_TIMEOUT_S;
        int seconds=0;
        if(sscanf(headers.timeout.c_str(),"Second-%d",&seconds)==1 && seconds>0) *granted=seconds;
    }
    return ESP_OK;
}

// Drains and discards the request body (bounded), then answers 200 and reports
// the service from the URI suffix. The SID is not checked: a speaker sends its
// first NOTIFY as soon as it answers SUBSCRIBE, often before the SID is stored
// here, and that first full-state event is the one that matters most. A stale
// subscription costs at most one extra refresh.
esp_err_t notify(httpd_req_t* req) {
    char scratch[512];
    size_t remaining=req->content_len, skipped=0;
    while(remaining && skipped<MAX_NOTIFY_BODY) {
        int n=httpd_req_recv(req,scratch,std::min(remaining,sizeof(scratch)));
        if(n<=0) break;
        remaining-=n; skipped+=n;
    }
    httpd_resp_send(req,nullptr,0);
    const char* service=strrchr(req->uri,'/');
    if(service && service[1] && callback) callback(service+1);
    return ESP_OK;
}

void ip_event(void*, esp_event_base_t, int32_t, void*) {
    if(bits) xEventGroupSetBits(bits,WAKE);
}

bool same_target(const EventTarget& a, const EventTarget& b) {
    return a.ip==b.ip && a.service==b.service;
}

// Reconcile subscriptions with the target list: unsubscribe removed targets,
// subscribe new ones, renew on schedule. Runs only on this task.
void maintenance(void*) {
    for(;;) {
        int64_t now=esp_timer_get_time(), next=now+30LL*1000000;
        auto ip=sta_ip();
        if(ip.empty()) {
            std::lock_guard<std::mutex> guard(lock);
            if(!subscriptions.empty()) {
                // Speakers cannot be reached; drop SIDs locally and let the
                // speakers' own timeouts expire them. Resubscribe once back.
                ESP_LOGW(TAG,"Wi-Fi down; dropping %zu subscription(s)",subscriptions.size());
                for(auto& sub:subscriptions) sub.sid.clear();
            }
        } else {
            { std::lock_guard<std::mutex> guard(lock);
                if(ip!=bound_ip) {
                    if(!bound_ip.empty()) ESP_LOGI(TAG,"local address changed to %s; resubscribing",ip.c_str());
                    for(auto& sub:subscriptions) sub.sid.clear();
                    bound_ip=ip;
                }
            }
            std::vector<EventTarget> wanted;
            { std::lock_guard<std::mutex> guard(lock); wanted=targets; }
            for(auto it=subscriptions.begin(); it!=subscriptions.end();) {
                bool kept=std::any_of(wanted.begin(),wanted.end(),[&](auto& t){return same_target(t,it->target);});
                if(kept) { ++it; continue; }
                Subscription removed=*it;
                { std::lock_guard<std::mutex> guard(lock); it=subscriptions.erase(it); }
                if(!removed.sid.empty()) {
                    gena(HTTP_METHOD_UNSUBSCRIBE,removed,"",nullptr,nullptr);
                    ESP_LOGI(TAG,"unsubscribed %s on %s",removed.target.service.c_str(),removed.target.ip.c_str());
                }
            }
            for(auto& t:wanted) {
                bool have=std::any_of(subscriptions.begin(),subscriptions.end(),[&](auto& s){return same_target(s.target,t);});
                if(!have) {
                    std::lock_guard<std::mutex> guard(lock);
                    subscriptions.emplace_back().target=t;
                }
            }
            for(size_t i=0;i<subscriptions.size();++i) {
                EventTarget target; std::string sid; bool due;
                { std::lock_guard<std::mutex> guard(lock);
                    auto& sub=subscriptions[i];
                    target=sub.target; sid=sub.sid;
                    due=sid.empty() || now>=sub.renew_at;
                    if(!due) next=std::min(next,sub.renew_at); }
                if(!due) continue;
                if(!sid.empty()) {
                    // Renew first; a failure means the SID is gone, so
                    // subscribe fresh rather than retrying the dead SID.
                    Subscription request;
                    request.target=target; request.sid=sid;
                    int granted=REQUESTED_TIMEOUT_S;
                    if(gena(HTTP_METHOD_SUBSCRIBE,request,"",nullptr,&granted)==ESP_OK) {
                        std::lock_guard<std::mutex> guard(lock);
                        subscriptions[i].renew_at=now+granted*500000LL;
                        next=std::min(next,subscriptions[i].renew_at);
                        ESP_LOGI(TAG,"renewed %s on %s",target.service.c_str(),target.ip.c_str());
                        continue;
                    }
                    std::lock_guard<std::mutex> guard(lock);
                    subscriptions[i].sid.clear();
                }
                auto callback_uri="http://"+ip+":"+std::to_string(PORT)+"/notify/"+target.service;
                std::string new_sid; int granted=REQUESTED_TIMEOUT_S;
                Subscription request; request.target=target;
                if(gena(HTTP_METHOD_SUBSCRIBE,request,callback_uri,&new_sid,&granted)==ESP_OK) {
                    std::lock_guard<std::mutex> guard(lock);
                    subscriptions[i].sid=new_sid;
                    subscriptions[i].renew_at=now+granted*500000LL;
                    next=std::min(next,subscriptions[i].renew_at);
                    ESP_LOGI(TAG,"subscribed %s on %s (SID %s, %d s)",target.service.c_str(),
                             target.ip.c_str(),new_sid.c_str(),granted);
                }
            }
        }
        int64_t delay=std::max<int64_t>(next-esp_timer_get_time(),100000);
        xEventGroupWaitBits(bits,WAKE,pdTRUE,pdFALSE,pdMS_TO_TICKS(delay/1000));
    }
}
}

void events_start(void (*on_event)(const char*)) {
    callback=on_event;
    bits=xEventGroupCreate();
    if(!bits) { ESP_LOGE(TAG,"allocation failed"); return; }
    httpd_config_t config=HTTPD_DEFAULT_CONFIG();
    config.server_port=PORT;
    config.uri_match_fn=httpd_uri_match_wildcard;
    config.max_uri_handlers=12; // /notify/* plus headroom for OTA and debug handlers
    config.lru_purge_enable=true;
    // OTA flash writes and screenshot encoding run in this task; the 4 KB
    // default overflowed during the first OTA upload on hardware.
    config.stack_size=12288;
    if(auto err=httpd_start(&server,&config); err!=ESP_OK) {
        ESP_LOGE(TAG,"server start failed: %s",esp_err_to_name(err));
        return;
    }
    httpd_uri_t uri{};
    uri.uri="/notify/*"; uri.method=HTTP_NOTIFY; uri.handler=notify;
    if(auto err=httpd_register_uri_handler(server,&uri); err!=ESP_OK) {
        ESP_LOGE(TAG,"NOTIFY handler registration failed: %s",esp_err_to_name(err));
        httpd_stop(server); server=nullptr;
        return;
    }
    esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,ip_event,nullptr);
    esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,ip_event,nullptr);
    if(xTaskCreate(maintenance,"events",4096,nullptr,4,nullptr)!=pdPASS)
        ESP_LOGE(TAG,"maintenance task failed to start");
    else
        ESP_LOGI(TAG,"listening for UPnP events on port %d",PORT);
}

void events_set_targets(const std::vector<EventTarget>& new_targets) {
    { std::lock_guard<std::mutex> guard(lock); targets=new_targets; }
    if(bits) xEventGroupSetBits(bits,WAKE);
}

httpd_handle_t events_server() { return server; }
