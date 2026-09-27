#pragma once
#include "sonos.hpp"
#include <string>
void network_init();
void network_connect(const std::string& ssid, const std::string& password);
bool network_online();
// Maximum modem sleep while nobody is looking; none while the screen is on,
// so incoming events are not held for up to three beacon intervals.
void network_power_save(bool save);
// Standby: stop the radio with no reconnect attempts, then start and rejoin
// the saved network (asynchronously; network_online() reports when done).
void network_suspend();
void network_resume();
std::string discover_speaker();
bool speaker_reachable(const std::string& ip, int timeout_ms);
std::string soap_http(const sonos::Request& request);
