#pragma once
#include "lvgl.h"
#include <cstdint>
#include <string>
// Backlight power: dim after inactivity, then off, then (display asleep)
// motion-wake only. Touch or motion wakes it.
void screen_init(void (*on_wake)());  // call with the display lock held, after the UI exists
void screen_note_motion();            // safe from any task
bool screen_off();
void screen_force_off(bool deep);     // debug: dark now, until touch or motion (display lock held)
// The touch panel is read only on its interrupt (event mode). A release read
// too soon after the interrupt still says "pressed", so LVGL keeps the finger
// down and the next tap looks like a drag from the old spot: a missed tap.
// While a press is held this polls every 15 ms until the release is seen.
void touch_guard_init(lv_indev_t* touch);  // display lock held
uint32_t touch_releases_polled();         // releases the interrupt path missed (for /perf)
std::string touch_trace();                // recent panel reads and input events (for /touch)
