#pragma once
// The touch UI. ui_build() runs with the display lock held; every other
// function takes the lock itself and is called from the worker task.
#include "app.hpp"
#include "artwork.hpp"
#include "power.hpp"
#include <utility>

enum class View { NowPlaying, Favorites, Queue, Rooms, Settings };

void ui_build();
void ui_show(View view);
View ui_view();
sonos::Room ui_selected();            // the room the controls act on
void ui_select(const std::string& room_id);

void ui_rooms(const std::vector<sonos::Room>& rooms);
void ui_favorites(const std::vector<sonos::Favorite>& favorites);
void ui_favorite_art(const std::string& favorite_id, Artwork art);
void ui_state(const sonos::Room& room, const sonos::State& state);
void ui_artwork(const sonos::Room& room, Artwork art);
void ui_queue(const sonos::Room& room, const std::vector<sonos::QueueItem>& items, int total, int track);
void ui_summaries(const std::vector<std::pair<std::string, sonos::Summary>>& by_coordinator);
void ui_areas(const std::vector<Area>& areas);
void ui_battery(const Battery& battery);
void ui_online(bool online);
void ui_toast(const std::string& text, bool error = false);
