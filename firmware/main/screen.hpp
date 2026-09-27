#pragma once
// Backlight power: dim after inactivity, then off, then (display asleep)
// motion-wake only. Touch or motion wakes it.
void screen_init(void (*on_wake)());  // call with the display lock held, after the UI exists
void screen_note_motion();            // safe from any task
bool screen_off();
void screen_force_off(bool deep);     // debug: dark now, until touch or motion (display lock held)
