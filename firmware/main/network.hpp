#pragma once
#include "sonos.hpp"
#include <string>
void network_init();
void network_connect(const std::string& ssid, const std::string& password);
bool network_online();
std::string discover_speaker();
std::string soap_http(const sonos::Request& request);
