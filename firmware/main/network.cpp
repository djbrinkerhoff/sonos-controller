#include "network.hpp"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <atomic>
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
const char* TAG="network";
constexpr EventBits_t ONLINE = 1, RETRY = 2;
EventGroupHandle_t events;
bool started = false;
std::atomic<bool> reconnect{false};
std::atomic<bool> connected_once{false};
void wifi_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        connected_once = true;
        xEventGroupSetBits(events, ONLINE);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, ONLINE);
        if (reconnect) xEventGroupSetBits(events, RETRY);
    }
}
// Reconnects off the event loop with capped exponential backoff, so a wrong
// password or an absent access point is retried politely rather than in a
// tight loop, and the controller still recovers when the network returns.
void reconnect_task(void*) {
    uint32_t backoff_ms = 1000;
    for (;;) {
        xEventGroupWaitBits(events, RETRY, pdTRUE, pdFALSE, portMAX_DELAY);
        if (connected_once.exchange(false)) backoff_ms = 1000;
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = std::min<uint32_t>(backoff_ms * 2, 60000);
        if (reconnect && !(xEventGroupGetBits(events) & ONLINE)) esp_wifi_connect();
    }
}
struct Body { std::string xml; bool overflow = false; };
esp_err_t http_event(esp_http_client_event_t* e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        auto& body = *static_cast<Body*>(e->user_data);
        if (body.xml.size() + e->data_len > 512 * 1024) {
            body.overflow = true;
            return ESP_FAIL;
        }
        body.xml.append(static_cast<char*>(e->data), e->data_len);
    }
    return ESP_OK;
}
void check(esp_err_t result, const char* context) {
    if (result != ESP_OK) throw std::runtime_error(std::string(context) + ": " + esp_err_to_name(result));
}
}
void network_init() {
    events = xEventGroupCreate();
    if (!events) throw std::runtime_error("Unable to allocate network state");
    check(esp_netif_init(),"Network init");
    check(esp_event_loop_create_default(),"Event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    check(esp_wifi_init(&config),"Wi-Fi init");
    check(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr),"Wi-Fi events");
    check(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr),"IP events");
    check(esp_wifi_set_storage(WIFI_STORAGE_RAM),"Wi-Fi storage");
    check(esp_wifi_set_mode(WIFI_MODE_STA),"Wi-Fi station mode");
    if (xTaskCreate(reconnect_task,"wifi_retry",3072,nullptr,3,nullptr)!=pdPASS)
        throw std::runtime_error("Unable to start Wi-Fi reconnect task");
}
bool network_online() { return events && (xEventGroupGetBits(events) & ONLINE); }
void network_connect(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size()>32 || password.size()>63) throw std::runtime_error("Check Wi-Fi name and password length");
    reconnect=false;
    if(started) { esp_wifi_disconnect(); check(esp_wifi_stop(),"Stop Wi-Fi"); }
    xEventGroupClearBits(events,ONLINE|RETRY);
    wifi_config_t config{};
    std::memcpy(config.sta.ssid,ssid.data(),ssid.size());
    std::memcpy(config.sta.password,password.data(),password.size());
    check(esp_wifi_set_config(WIFI_IF_STA,&config),"Wi-Fi configuration");
    reconnect=true;
    check(esp_wifi_start(),"Start Wi-Fi"); started=true;
    // Power save lets the C6 sleep between beacons and was a suspect in the
    // intermittent SSDP discovery. Revisit together with battery/sleep work.
    if (auto ps=esp_wifi_set_ps(WIFI_PS_NONE); ps!=ESP_OK) ESP_LOGW(TAG,"Wi-Fi power save unchanged: %s",esp_err_to_name(ps));
    check(esp_wifi_connect(),"Connect Wi-Fi");
    if (!(xEventGroupWaitBits(events,ONLINE,pdFALSE,pdFALSE,pdMS_TO_TICKS(20000)) & ONLINE))
        throw std::runtime_error("Wi-Fi connection timed out. Check credentials and 2.4 GHz coverage.");
}
// A bounded TCP connect: lwIP does not apply SO_SNDTIMEO to connect(), so use
// a non-blocking connect and wait for it with select().
bool speaker_reachable(const std::string& ip, int timeout_ms) {
    int fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(fd<0) { ESP_LOGE(TAG,"probe: socket() failed errno=%d",errno); return false; }
    struct Cleanup { int fd; ~Cleanup(){close(fd);} } cleanup{fd};
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
    sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_port=htons(1400);
    if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1) return false;
    int error=0;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) {
        if(errno!=EINPROGRESS) error=errno;
        else {
            fd_set writable; FD_ZERO(&writable); FD_SET(fd,&writable);
            timeval timeout{timeout_ms/1000,(timeout_ms%1000)*1000};
            int ready=select(fd+1,nullptr,&writable,nullptr,&timeout);
            socklen_t len=sizeof(error);
            if(ready<=0) error=ready==0?ETIMEDOUT:errno;
            else getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&len);
        }
    }
    if(error) ESP_LOGW(TAG,"probe: TCP %s:1400 unreachable errno=%d (%s)",ip.c_str(),error,strerror(error));
    return error==0;
}

std::string soap_http(const sonos::Request& request) {
    if (!network_online()) throw std::runtime_error("Wi-Fi is disconnected");
    const auto url="http://"+request.ip+":1400"+request.path;
    Body body;
    esp_http_client_config_t config{};
    // Applies to each connect/read, not the whole transfer. The earlier 30 s
    // value masked an SDIO misconfiguration that has since been fixed.
    config.url=url.c_str(); config.timeout_ms=5000;
    config.event_handler=http_event; config.user_data=&body;
    config.disable_auto_redirect=true;
    config.buffer_size=4096;
    auto client=esp_http_client_init(&config);
    if(!client) throw std::runtime_error("HTTP allocation failed");
    struct Cleanup { esp_http_client_handle_t handle; ~Cleanup(){esp_http_client_cleanup(handle);} } cleanup{client};
    check(esp_http_client_set_method(client,HTTP_METHOD_POST),"HTTP method");
    check(esp_http_client_set_header(client,"Content-Type","text/xml; charset=\"utf-8\""),"HTTP content type");
    auto action="\""+request.service+"#"+request.action+"\"";
    check(esp_http_client_set_header(client,"SOAPACTION",action.c_str()),"SOAP action");
    check(esp_http_client_set_post_field(client,request.body.data(),request.body.size()),"SOAP body");
    auto result=esp_http_client_perform(client);
    if(result!=ESP_OK) {
        ESP_LOGE(TAG,"soap: %s:%d %s failed: %s (received %zu bytes, status %d)",
                 request.ip.c_str(),1400,request.action.c_str(),esp_err_to_name(result),
                 body.xml.size(),esp_http_client_get_status_code(client));
        // Separates "the network cannot reach the speaker" from "the HTTP
        // client misbehaved", which ESP_ERR_HTTP_CONNECT alone cannot.
        if(speaker_reachable(request.ip,2000)) ESP_LOGI(TAG,"probe: TCP %s:1400 reachable",request.ip.c_str());
    }
    if(body.overflow) throw std::runtime_error("Speaker response exceeds device memory limit");
    check(result,"Speaker request failed; state will refresh before another action");
    int status=esp_http_client_get_status_code(client);
    if(status!=200 && status!=500) throw std::runtime_error("Speaker HTTP error "+std::to_string(status));
    return body.xml; // SOAP faults are parsed by the shared client.
}
std::string discover_speaker() {
    int fd=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(fd<0) throw std::runtime_error("Cannot open discovery socket");
    struct Cleanup { int fd; ~Cleanup(){close(fd);} } cleanup{fd};
    timeval timeout{0,250000};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_port=htons(1900);
    inet_pton(AF_INET,"239.255.255.250",&address.sin_addr);
    const std::string request="M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    // Speakers answer after a random delay of up to MX seconds, so listen
    // against one overall deadline and resend in case a datagram was lost,
    // instead of giving up at the first quiet interval.
    const int64_t start=esp_timer_get_time();
    const int64_t deadline=start+4000000;
    int64_t next_send=start;
    int sent=0;
    while(esp_timer_get_time()<deadline) {
        if(sent<3 && esp_timer_get_time()>=next_send) {
            sendto(fd,request.data(),request.size(),0,reinterpret_cast<sockaddr*>(&address),sizeof(address));
            ++sent; next_send+=1000000;
        }
        char data[4096];
        int n=recv(fd,data,sizeof(data)-1,0);
        if(n<=0) continue;
        std::string response(data,n), lower=response;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
        if(lower.find("zoneplayer")==std::string::npos) continue;
        auto begin=lower.find("\r\nlocation:");
        if(begin==std::string::npos) continue;
        begin+=11;
        while(begin<response.size() && response[begin]==' ') ++begin;
        auto ip=sonos::ipv4_from_url(response.substr(begin,response.find("\r\n",begin)-begin));
        if(!ip.empty()) {
            ESP_LOGI(TAG,"ssdp: found %s after %lld ms, %d request(s)",ip.c_str(),(esp_timer_get_time()-start)/1000,sent);
            return ip;
        }
    }
    throw std::runtime_error("No Sonos speaker found. Enter a speaker IP in Settings.");
}
