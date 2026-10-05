#pragma once

// Battery charger pins, INA226 pack monitor and BMI270 wake-on-motion.
// power_init() must run after bsp_feature_enable(BSP_FEATURE_WIFI, true):
// the BSP's io_expander1 creation resets the PI4IOE5V6408, which silently
// turns every pin into an input and stops the battery from charging.
void power_init();

struct Battery {
    bool valid = false;     // false: the monitor could not be read; keep the last reading
    bool present = false;
    int pack_mv = 0;
    int percent = 0;        // load-corrected and smoothed (battery_model.hpp)
    int current_ma = 0;     // positive while charging (M5Unified convention)
    bool charging = false;  // current flowing into the pack
    bool external = false;  // on USB power with the charger idle: full, or no pack
};
Battery battery_read();
// On USB power (charging or full), from the latest battery_read(). Screen-off
// tiers that only motion can wake are skipped then: a docked Tab5 should
// still answer a tap.
bool power_external();
// Mean pack discharge current (mA) over the window, for power measurements.
int power_measure_ma(int seconds, int* low = nullptr, int* high = nullptr);

// Starts a low-priority IMU poll task; on_motion runs on that task and must
// be cheap. Logs and returns without a task if the BMI270 is absent.
void motion_start(void (*on_motion)());
bool motion_active();
void motion_pause(bool paused);
// Light-sleeps the chip until the device moves or is touched, or limit_ms
// passes (0: no limit), then calls the motion callback so the screen wakes.
// Stop Wi-Fi first. Blocks the caller; returns what woke it.
const char* power_standby(int limit_ms = 0);
