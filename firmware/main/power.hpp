#pragma once

// Battery charger pins, INA226 pack monitor and BMI270 wake-on-motion.
// power_init() must run after bsp_feature_enable(BSP_FEATURE_WIFI, true):
// the BSP's io_expander1 creation resets the PI4IOE5V6408, which silently
// turns every pin into an input and stops the battery from charging.
void power_init();

struct Battery {
    bool present = false;
    int pack_mv = 0;
    int percent = 0;
    int current_ma = 0;  // positive while charging (M5Unified convention)
    bool charging = false;
};
Battery battery_read();

// Starts a low-priority IMU poll task; on_motion runs on that task and must
// be cheap. Logs and returns without a task if the BMI270 is absent.
void motion_start(void (*on_motion)());
bool motion_active();
