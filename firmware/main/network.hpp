#pragma once
#include "sonos.hpp"
#include <string>
void network_init();
void network_connect(const std::string& ssid, const std::string& password);
bool network_online();
std::string discover_speaker();
bool speaker_reachable(const std::string& ip, int timeout_ms);
std::string soap_http(const sonos::Request& request);
