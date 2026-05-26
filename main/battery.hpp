// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <optional>

#include <utility/power/INA226_Class.hpp>

namespace stackchan::app {

// One battery sample from the base-board INA226 (I2C 0x41).
struct BatteryReading {
    float voltage; // bus voltage [V] (= battery voltage)
    float current; // shunt current [A] (sign per INA226 wiring)
};

// Thin wrapper over m5::INA226_Class for the Stack-chan base-board battery
// monitor. All I2C access goes through m5::In_I2C, so begin()/read() must be
// called from the task that owns the internal bus (demo_loop / app_main),
// never concurrently with M5.update() / touch reads.
class BatteryMonitor {
public:
    // Configure (shunt 0.01Ω, max 8.19A — matches StackChan-BSP) and start the
    // INA226. Returns false if the chip didn't respond (older base, no battery
    // monitor); read() then always returns nullopt.
    bool begin();

    // Latest bus voltage + shunt current, or nullopt if begin() failed.
    std::optional<BatteryReading> read();

private:
    m5::INA226_Class ina_{0x41};
    bool ok_ = false;
};

// Map a single-cell LiPo bus voltage [V] to a 0..100 % estimate using a
// piecewise-linear discharge curve (clamped outside 3.30..4.20 V).
int battery_percent_from_voltage(float voltage);

} // namespace stackchan::app
