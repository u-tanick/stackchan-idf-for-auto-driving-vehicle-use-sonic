// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "battery.hpp"

#include <array>

#include <esp_log.h>

namespace stackchan::app {

namespace {
constexpr const char* kTag = "battery";

// Single-cell LiPo discharge curve: {voltage [V], percent}. Descending by
// voltage; linearly interpolated between points, clamped at the ends.
struct CurvePoint {
    float v;
    int pct;
};
constexpr std::array<CurvePoint, 10> kCurve{{
    {4.20f, 100}, {4.10f, 90}, {4.00f, 80}, {3.90f, 65}, {3.80f, 55},
    {3.70f, 45}, {3.60f, 30}, {3.50f, 18}, {3.40f, 8}, {3.30f, 0},
}};
} // namespace

bool BatteryMonitor::begin()
{
    m5::INA226_Class::config_t cfg;
    cfg.shunt_res = 0.01f;            // base-board shunt resistor
    cfg.max_expected_current = 8.19f; // matches StackChan-BSP
    ina_.config(cfg);
    ok_ = ina_.begin();
    if (!ok_) {
        ESP_LOGW(kTag, "INA226 not found at 0x41 (battery monitoring disabled)");
    } else {
        ESP_LOGI(kTag, "INA226 battery monitor ready");
    }
    return ok_;
}

std::optional<BatteryReading> BatteryMonitor::read()
{
    if (!ok_) {
        return std::nullopt;
    }
    return BatteryReading{ina_.getBusVoltage(), ina_.getShuntCurrent()};
}

int battery_percent_from_voltage(float voltage)
{
    if (voltage >= kCurve.front().v) return kCurve.front().pct;
    if (voltage <= kCurve.back().v) return kCurve.back().pct;
    for (std::size_t i = 1; i < kCurve.size(); ++i) {
        if (voltage >= kCurve[i].v) {
            const CurvePoint& hi = kCurve[i - 1];
            const CurvePoint& lo = kCurve[i];
            const float t = (voltage - lo.v) / (hi.v - lo.v);
            return lo.pct + static_cast<int>((hi.pct - lo.pct) * t + 0.5f);
        }
    }
    return kCurve.back().pct;
}

} // namespace stackchan::app
