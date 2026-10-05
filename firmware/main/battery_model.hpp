#pragma once
// Battery state of charge from the INA226's pack voltage and current. Plain
// C++ (no ESP-IDF) so it is tested natively (tests/battery_test.cpp).
//
// Three corrections over reading the raw voltage on a straight line:
// - Load: the pack's internal resistance shifts the voltage by I*R (about
//   0.19 ohm, measured on this pack: 8058 mV at 231 mA discharge and
//   8290 mV at 1006 mA charge, 40 s apart). Uncorrected, a 1 A charge read
//   100% at about 84%.
// - Shape: a Li-ion cell's voltage is flat through the middle and drops off
//   at the end, so a resting-voltage curve replaces the line.
// - Noise: readings are smoothed, and the shown percent only falls while
//   discharging and only rises while charging.
#include <cstdint>

enum class PowerSource { Battery, Charging, External };  // External: on USB, not charging (full)

struct BatteryReading {
    int pack_mv = 0;
    int current_ma = 0;  // positive while charging
};

class BatteryModel {
public:
    static constexpr int INTERNAL_MOHM = 190;
    static constexpr int CURRENT_DEADBAND_MA = 20;  // on battery the device always draws more
    static constexpr int MIN_PACK_MV = 5000;        // below a 2-cell pack's floor: no pack
    // Resting per-cell voltage -> percent. 100% is where this pack settles
    // after the charger stops (4.15 V; it charges to 4.19 V). Generic Li-ion
    // shape between the measured ends; a full logged discharge would refine it.
    static int percent_for_cell_mv(int cell_mv);
    static int resting_cell_mv(const BatteryReading& r);
    static PowerSource source_for(const BatteryReading& r);

    // Feeds one reading; returns false if no pack is present.
    bool update(const BatteryReading& r);
    int percent() const { return shown_; }
    PowerSource source() const { return source_; }
    bool known() const { return shown_ >= 0; }

private:
    double smoothed_ = -1;  // percent, exponentially smoothed
    int shown_ = -1;
    PowerSource source_ = PowerSource::Battery;
};
