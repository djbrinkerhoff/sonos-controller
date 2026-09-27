#pragma once
// Shared between the UI (LVGL task) and the controller worker.
#include "sonos.hpp"
#include <string>
#include <vector>

struct Area { std::string name; std::vector<std::string> ids; };

struct Command {
    std::string action, ssid, password, seed, name;
    sonos::Room room;
    sonos::Favorite favorite;
    int value = 0;
    std::vector<std::string> area_ids;
};

// Hands a command to the worker, taking ownership. Returns false (and deletes
// it) if the worker is still busy with earlier commands.
bool submit(Command* command);

std::string setting(const char* key);  // stored text setting, "" if absent
