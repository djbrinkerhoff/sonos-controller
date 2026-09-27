#pragma once
#include "lvgl.h"
// Replaces the LVGL port's flush with one where the PPA rotates each rendered
// band straight into the panel framebuffer, without blocking. Returns false
// (and leaves the port's flush in place) if anything is unavailable.
bool fast_flush_install(lv_display_t* display);
// The panel's framebuffer (native 720x1280 RGB565), or nullptr; for /panel.
const uint16_t* fast_flush_framebuffer();
uint32_t fast_flush_lost_completions();  // PPA completions that timed out; should stay 0
