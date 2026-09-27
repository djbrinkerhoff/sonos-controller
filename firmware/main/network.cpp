#include "network.hpp"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace {
EventGroupHandle_t events;
bool started = false;
bool reconnect = true;
void wifi_event(void*, esp_event_base_t base, int32_t id, void*) {
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(events, 1);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(events, 1);
        if (reconnect) esp_wifi_connect();
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
}
bool network_online() { return events && (xEventGroupGetBits(events) & 1); }
void network_connect(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || ssid.size()>32 || password.size()>63) throw std::runtime_error("Check Wi-Fi name and password length");
    reconnect=false;
    if(started) { esp_wifi_disconnect(); check(esp_wifi_stop(),"Stop Wi-Fi"); }
    xEventGroupClearBits(events,1);
    wifi_config_t config{};
    std::memcpy(config.sta.ssid,ssid.data(),ssid.size());
    std::memcpy(config.sta.password,password.data(),password.size());
    check(esp_wifi_set_config(WIFI_IF_STA,&config),"Wi-Fi configuration");
    reconnect=true;
    check(esp_wifi_start(),"Start Wi-Fi"); started=true;
    check(esp_wifi_connect(),"Connect Wi-Fi");
    if (!(xEventGroupWaitBits(events,1,pdFALSE,pdFALSE,pdMS_TO_TICKS(20000)) & 1))
        throw std::runtime_error("Wi-Fi connection timed out. Check credentials and 2.4 GHz coverage.");
}
std::string soap_http(const sonos::Request& request) {
    if (!network_online()) throw std::runtime_error("Wi-Fi is disconnected");
    const auto url="http://"+request.ip+":1400"+request.path;
    Body body;
    esp_http_client_config_t config{};
    config.url=url.c_str(); config.timeout_ms=3000;
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
    timeval timeout{1,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    sockaddr_in address{};
    address.sin_family=AF_INET; address.sin_port=htons(1900);
    inet_pton(AF_INET,"239.255.255.250",&address.sin_addr);
    const std::string request="M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    for(int attempt=0;attempt<3;++attempt) {
        sendto(fd,request.data(),request.size(),0,reinterpret_cast<sockaddr*>(&address),sizeof(address));
        for(int packet=0;packet<64;++packet) {
            char data[4096];
            int n=recv(fd,data,sizeof(data)-1,0);
            if(n<=0) break;
            std::string response(data,n), lower=response;
            std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
            if(lower.find("zoneplayer")==std::string::npos) continue;
            auto begin=lower.find("\r\nlocation:");
            if(begin==std::string::npos) continue;
            begin+=11;
            while(begin<response.size() && response[begin]==' ') ++begin;
            auto ip=sonos::ipv4_from_url(response.substr(begin,response.find("\r\n",begin)-begin));
            if(!ip.empty()) return ip;
        }
    }
    throw std::runtime_error("No Sonos speaker found. Enter a speaker IP in Settings.");
}
