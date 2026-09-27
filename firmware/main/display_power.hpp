#pragma once
// Panel power for a long-dark screen. Asleep, the panel is switched off and
// (on the ST7121 panel) the DPI driver is deleted: that stops the 110 MB/s
// framebuffer stream out of PSRAM and releases the driver's max-CPU-frequency
// lock, so the CPU can drop to 40 MHz. Waking recreates the driver, which
// starts the stream at the top of a frame. The touch controller shares the
// panel's chip and stops scanning without the stream, so only motion wakes a
// sleeping display. Call with the display lock held.
#include "lvgl.h"

void display_power_init(lv_display_t* display);  // after fast_flush_install()
void display_sleep();
void display_wake();       // returns with the screen redrawn, ready for the backlight
bool display_asleep();
