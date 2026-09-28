// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/ltr553.hpp"

#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board {

namespace {
constexpr const char* kTag = "ltr553";
} // namespace

bool Ltr553Proximity::write_reg(std::uint8_t reg, std::uint8_t val)
{
    return m5::In_I2C.writeRegister8(kAddress, reg, val, kI2cFreq);
}

bool Ltr553Proximity::read_regs(std::uint8_t reg, std::uint8_t* buf, std::size_t len)
{
    return m5::In_I2C.readRegister(kAddress, reg, buf, len, kI2cFreq);
}

std::optional<Ltr553Proximity> Ltr553Proximity::probe()
{
    // Check if LTR-553 responds at address 0x23 on internal I2C bus
    if (!m5::In_I2C.scanID(kAddress, kI2cFreq)) {
        return std::nullopt;
    }

    // Configure LTR-553ALS-WA
    // 0x80: ALS_CONTR -> Active mode (bit 0 = 1)
    if (!write_reg(kRegAlsContr, 0x01)) {
        ESP_LOGW(kTag, "Failed to write ALS_CONTR (0x80)");
        return std::nullopt;
    }

    // 0x81: PS_CONTR -> Active mode (bit 1 = 1), PS Saturation indicator enable (bit 0 = 1) -> 0x03
    if (!write_reg(kRegPsContr, 0x03)) {
        ESP_LOGW(kTag, "Failed to write PS_CONTR (0x81)");
        return std::nullopt;
    }

    // 0x82: PS_LED -> 60kHz pulse freq, 100% duty, 100mA current -> 0x7F
    write_reg(kRegPsLed, 0x7F);

    // 0x83: PS_N_PULSES -> 1 pulse
    write_reg(kRegPsNPulses, 0x01);

    // 0x84: PS_MEAS_RATE -> 50ms measurement rate (0x00: 10ms, 0x01: 50ms, 0x02: 100ms)
    write_reg(kRegPsMeasRate, 0x01);

    ESP_LOGI(kTag, "LTR-553ALS-WA initialized on internal I2C at 0x%02X", kAddress);
    return Ltr553Proximity{};
}

std::optional<Ltr553Proximity::Reading> Ltr553Proximity::read(std::uint16_t threshold)
{
    static std::optional<Ltr553Proximity> s_instance;
    static bool s_probe_attempted = false;

    if (!s_instance && !s_probe_attempted) {
        s_probe_attempted = true;
        s_instance = probe();
    }

    if (!s_instance) {
        return std::nullopt;
    }
    return s_instance->read_proximity(threshold);
}

std::optional<Ltr553Proximity::Reading> Ltr553Proximity::read_raw(std::uint16_t threshold)
{
    // Read 2 bytes from PS_DATA_0 (0x8D) and PS_DATA_1 (0x8E)
    std::uint8_t data[2] = {0, 0};
    if (!read_regs(kRegPsData0, data, 2)) {
        return std::nullopt;
    }

    Reading r;
    // 11-bit ADC: lower 8 bits in data[0], upper 3 bits in data[1][2:0]
    r.ps_raw = (static_cast<std::uint16_t>(data[1] & 0x07) << 8) | data[0];
    r.saturated = (data[1] & 0x80) != 0;
    r.obstacle_near = (r.ps_raw >= threshold) || r.saturated;

    return r;
}

std::optional<Ltr553Proximity::Reading> Ltr553Proximity::read_proximity(std::uint16_t threshold)
{
    // Take multiple readings across ~50ms to ensure stable proximity data
    Reading best{};
    bool got_any = false;

    for (int i = 0; i < 3; ++i) {
        if (auto r = read_raw(threshold); r) {
            got_any = true;
            if (r->ps_raw > best.ps_raw) {
                best = *r;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (!got_any) {
        return std::nullopt;
    }
    return best;
}

} // namespace stackchan::board
