#pragma once
// Survives a panic: the next boot logs where the previous run crashed.
void crash_report();  // call early in app_main, after logging is set up
