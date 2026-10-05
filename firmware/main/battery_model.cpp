#include "battery_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>

namespace {
struct Point { int cell_mv, percent; };
// Resting voltage of one Li-ion cell against charge, ascending.
constexpr Point CURVE[] = {
    {3300, 0},  {3500, 3},  {3600, 7},  {3650, 12}, {3690, 18}, {3720, 25},
    {3750, 32}, {3780, 40}, {3810, 48}, {3850, 56}, {3900, 65}, {3950, 73},
    {4000, 80}, {4050, 87}, {4100, 94}, {4150, 100},
};
constexpr double SMOOTHING = 0.3;  // weight of each new reading (taken every 10 s)
constexpr int JUMP = 5;            // a reading this far against the trend is believed at once
}

int BatteryModel::percent_for_cell_mv(int cell_mv) {
    if (cell_mv <= CURVE[0].cell_mv) return 0;
    if (cell_mv >= std::rbegin(CURVE)->cell_mv) return 100;
    auto hi = std::find_if(std::begin(CURVE), std::end(CURVE), [&](const Point& p) { return p.cell_mv >= cell_mv; });
    auto lo = std::prev(hi);
    return lo->percent + (cell_mv - lo->cell_mv) * (hi->percent - lo->percent) / (hi->cell_mv - lo->cell_mv);
}

int BatteryModel::resting_cell_mv(const BatteryReading& r) {
    // Charging pushes the terminal voltage up by I*R, discharging pulls it down.
    return (r.pack_mv - r.current_ma * INTERNAL_MOHM / 1000) / 2;
}

PowerSource BatteryModel::source_for(const BatteryReading& r) {
    if (r.current_ma > CURRENT_DEADBAND_MA) return PowerSource::Charging;
    if (r.current_ma < -CURRENT_DEADBAND_MA) return PowerSource::Battery;
    return PowerSource::External;
}

bool BatteryModel::update(const BatteryReading& r) {
    if (r.pack_mv < MIN_PACK_MV) { smoothed_ = -1; shown_ = -1; return false; }
    source_ = source_for(r);
    const int estimate = percent_for_cell_mv(resting_cell_mv(r));
    smoothed_ = smoothed_ < 0 ? estimate : smoothed_ + SMOOTHING * (estimate - smoothed_);
    const int target = static_cast<int>(std::lround(smoothed_));
    if (shown_ < 0 || std::abs(estimate - shown_) >= JUMP * 4) {
        // First reading, or a pack swap / bad earlier reading: start over.
        smoothed_ = estimate;
        shown_ = estimate;
    } else if (source_ == PowerSource::Battery) {
        if (target < shown_ || target > shown_ + JUMP) shown_ = target;  // never creeps up on battery
    } else if (source_ == PowerSource::Charging) {
        if (target > shown_ || target < shown_ - JUMP) shown_ = target;  // never creeps down while charging
    } else {
        shown_ = target;
    }
    // Still taking current means not yet full; the charger stopping is what 100% means.
    if (source_ == PowerSource::Charging) shown_ = std::min(shown_, 99);
    if (source_ == PowerSource::External && resting_cell_mv(r) >= 4100) shown_ = 100;
    shown_ = std::clamp(shown_, 0, 100);
    return true;
}
