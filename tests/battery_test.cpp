// Battery model tests. Readings come from this device's logs (October 2026).
#include "battery_model.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int checks = 0;
void expect(bool condition, const std::string& message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::exit(1); }
}
BatteryReading at(int mv, int ma) { return {mv, ma}; }

void curve() {
    expect(BatteryModel::percent_for_cell_mv(3000) == 0 && BatteryModel::percent_for_cell_mv(3300) == 0, "empty end");
    expect(BatteryModel::percent_for_cell_mv(4150) == 100 && BatteryModel::percent_for_cell_mv(4300) == 100, "full end");
    expect(BatteryModel::percent_for_cell_mv(3700) < 30, "3.70 V is low, not the straight line's 47%");
    expect(BatteryModel::percent_for_cell_mv(4025) == 83, "interpolates between points");
    int last = -1;
    for (int mv = 3200; mv <= 4250; mv += 5) {
        const int p = BatteryModel::percent_for_cell_mv(mv);
        expect(p >= last, "curve is monotonic at " + std::to_string(mv) + " mV");
        last = p;
    }
}

void load_correction() {
    // The same pack 40 s apart: on battery, then on the charger.
    const int on_battery = BatteryModel::resting_cell_mv(at(8058, -231));
    const int charging = BatteryModel::resting_cell_mv(at(8290, 1006));
    expect(std::abs(on_battery - charging) <= 5, "load correction makes both readings agree");
    // The old formula showed this charging reading as 100%.
    BatteryModel m;
    m.update(at(8331, 1005));
    expect(m.percent() >= 85 && m.percent() <= 92, "1 A charge at 8331 mV is about 89%, got " + std::to_string(m.percent()));
}

void sources() {
    expect(BatteryModel::source_for(at(8300, 171)) == PowerSource::Charging, "charging");
    expect(BatteryModel::source_for(at(8058, -231)) == PowerSource::Battery, "discharging");
    expect(BatteryModel::source_for(at(8292, -1)) == PowerSource::External, "about 0 mA on USB is external power");
}

void charge_then_full() {
    BatteryModel m;
    // Constant-voltage end of a charge: high voltage, tapering current.
    for (int ma : {246, 216, 193, 171}) {
        m.update(at(8377, ma));
        expect(m.percent() == 99, "still charging, so not 100% yet");
        expect(m.source() == PowerSource::Charging, "charging while current flows");
    }
    m.update(at(8322, -1));  // the charger stopped
    expect(m.percent() == 100 && m.source() == PowerSource::External, "charge done reads 100% on external power");
    m.update(at(8292, -1));  // relaxing
    expect(m.percent() == 100, "stays full while relaxing on USB");
}

void discharge_never_rises() {
    BatteryModel m;
    m.update(at(7700, -150));
    const int start = m.percent();
    int shown = start;
    // Noisy readings as the backlight and Wi-Fi change the load.
    const int noise[][2] = {{7690, -240}, {7720, -90}, {7680, -250}, {7710, -110}, {7670, -230}, {7700, -100}};
    for (auto& n : noise) {
        m.update(at(n[0], n[1]));
        expect(m.percent() <= shown, "percent rose on battery: " + std::to_string(shown) + " -> " + std::to_string(m.percent()));
        shown = m.percent();
    }
    expect(start - shown <= 4, "noise alone does not drain the gauge");
}

void unplugging_keeps_continuity() {
    BatteryModel m;
    m.update(at(8322, -1));
    expect(m.percent() == 100, "full on USB");
    m.update(at(8150, -240));  // unplugged, backlight on
    expect(m.percent() >= 95, "unplugging a full pack does not drop the gauge, got " + std::to_string(m.percent()));
}

void absent_and_swap() {
    BatteryModel m;
    expect(!m.update(at(4200, 0)) && !m.known(), "no pack below 5 V");
    m.update(at(7300, -200));
    const int low = m.percent();
    m.update(at(8250, -200));  // a charged pack swapped in
    expect(m.percent() - low >= 40, "a pack swap is believed at once");
}
}

int main() {
    curve();
    load_correction();
    sources();
    charge_then_full();
    discharge_never_rises();
    unplugging_keeps_continuity();
    absent_and_swap();
    std::printf("battery tests passed (%d checks)\n", checks);
    return 0;
}
