#pragma once
// Backlight power: dim after inactivity, then off. Touch or motion wakes it.
void screen_init(void (*on_wake)());  // call with the display lock held, after the UI exists
void screen_note_motion();            // safe from any task
bool screen_off();
