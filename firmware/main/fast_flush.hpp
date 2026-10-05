#pragma once
#include "lvgl.h"
#include "esp_lcd_types.h"
// Replaces the LVGL port's flush with one where the PPA rotates each rendered
// band straight into the panel framebuffer, without blocking. Returns false
// (and leaves the port's flush in place) if anything is unavailable.
bool fast_flush_install(lv_display_t* display);
// The panel's framebuffer (native 720x1280 RGB565), or nullptr; for /panel.
const uint16_t* fast_flush_framebuffer();
uint32_t fast_flush_lost_completions();  // PPA completions that timed out; should stay 0
// Switches to CPU rotation for good, as a wedged PPA does; call with the
// display lock held. For exercising that path (/ppa?cpu=1).
void fast_flush_force_cpu();
// While suspended, flushes are dropped: the panel driver may be deleted.
// Resume takes the (possibly recreated) panel; false if it has no framebuffer.
void fast_flush_suspend();
bool fast_flush_resume(esp_lcd_panel_handle_t panel);
// The panel and its command channel, for display power control.
esp_lcd_panel_handle_t fast_flush_panel();
esp_lcd_panel_io_handle_t fast_flush_panel_io();
