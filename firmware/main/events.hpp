#pragma once
#include "esp_http_server.h"
#include <string>
#include <vector>

struct EventTarget {
    std::string ip; std::string service;
    bool operator==(const EventTarget& o) const { return ip==o.ip && service==o.service; }
    bool operator!=(const EventTarget& o) const { return !(*this==o); }
};

// Starts the local HTTP server on port 3400 and the subscription maintenance
// task. on_event is invoked from the server task with the service name on each
// NOTIFY. Never throws; a failed start is only logged.
void events_start(void (*on_event)(const char* service));

// Replaces the set of speakers/services to stay subscribed to. Thread-safe;
// the maintenance task unsubscribes removed targets and subscribes new ones.
void events_set_targets(const std::vector<EventTarget>& targets);

// The shared server, so other modules (OTA, debug) can register handlers.
httpd_handle_t events_server();
