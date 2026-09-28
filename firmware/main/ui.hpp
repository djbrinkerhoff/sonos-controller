#pragma once
// The touch UI. ui_build() runs with the display lock held; every other
// function takes the lock itself and is called from the worker task.
#include "app.hpp"
#include "artwork.hpp"
#include "power.hpp"
#include <utility>

enum class View { NowPlaying, Favorites, Queue, Rooms, Settings };

// Artwork geometry. The worker prepares covers at exactly these sizes with the
// corners pre-rounded, so drawing them is a plain copy: scaling or clipping
// images in LVGL costs about a second per full-screen frame on this device.
namespace art_spec {
constexpr uint32_t now_side=480, now_radius=24;
// 196 (not 195): a 392-byte row keeps every image row 4-byte aligned. LVGL's
// copy loop falls back to byte-at-a-time copies on misaligned rows.
constexpr uint32_t tile_side=196, tile_radius=16;
constexpr uint32_t background=0x0F1216;  // the view background behind covers
}

void ui_build();
void ui_show(View view);
View ui_view();
sonos::Room ui_selected();            // the room the controls act on
void ui_select(const std::string& room_id);

void ui_rooms(const std::vector<sonos::Room>& rooms);
void ui_favorites(const std::vector<sonos::Favorite>& favorites);
// index is the favorite's position in the last ui_favorites list; the id is
// checked too so art cannot land on a different favorite after a reload.
void ui_favorite_art(const std::string& favorite_id, size_t index, Artwork art);
void ui_state(const sonos::Room& room, const sonos::State& state);
void ui_artwork(const sonos::Room& room, Artwork art);
void ui_queue(const sonos::Room& room, const std::vector<sonos::QueueItem>& items, int total, int track);
void ui_summaries(const std::vector<std::pair<std::string, sonos::Summary>>& by_coordinator);
void ui_areas(const std::vector<Area>& areas);
void ui_battery(const Battery& battery);
void ui_online(bool online);
void ui_toast(const std::string& text, bool error = false);
