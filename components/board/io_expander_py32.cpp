// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/io_expander_py32.hpp"

#include <M5Unified.h>
#include <esp_log.h>

namespace stackchan::board {

namespace {

constexpr std::uint8_t kRegVersion = 0x02;
constexpr std::uint8_t kRegGpioModeLow = 0x03;
constexpr std::uint8_t kRegGpioModeHigh = 0x04;
constexpr std::uint8_t kRegGpioOutLow = 0x05;
constexpr std::uint8_t kRegGpioOutHigh = 0x06;
constexpr std::uint8_t kRegGpioPullUpLow = 0x09;
constexpr std::uint8_t kRegGpioPullUpHigh = 0x0A;
constexpr std::uint8_t kRegGpioPullDownLow = 0x0B;
constexpr std::uint8_t kRegGpioPullDownHigh = 0x0C;
// LED strip on the M5 base. The PY32 owns the NeoPixel timing — the host
// just writes a count + RGB565 LE RAM and toggles the refresh bit.
constexpr std::uint8_t kRegLedCfg = 0x24;          // [5:0]=count, [6]=refresh
constexpr std::uint8_t kRegLedRamStart = 0x30;     // 32 LEDs × 2 B each
constexpr std::uint8_t kLedCfgRefreshBit = 1u << 6;
constexpr std::uint8_t kLedCfgCountMask = 0x3F;

} // namespace

tl::expected<Py32Expander, Error> Py32Expander::probe(std::uint8_t address)
{
    const std::uint8_t version = m5::In_I2C.readRegister8(address, kRegVersion, kI2cFreq);
    if (version == 0x00 || version == 0xFF) {
        return tl::unexpected{Error::ExpanderProbe};
    }
    return Py32Expander{address};
}

tl::expected<void, Error> Py32Expander::write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value)
{
    const std::uint8_t reg = (pin < 8) ? reg_l : reg_h;
    const std::uint8_t mask = static_cast<std::uint8_t>(1u << (pin & 0x7));

    const std::uint8_t current = m5::In_I2C.readRegister8(address_, reg, kI2cFreq);
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask) : static_cast<std::uint8_t>(current & ~mask);
    if (!m5::In_I2C.writeRegister8(address_, reg, next, kI2cFreq)) {
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

tl::expected<void, Error> Py32Expander::set_direction(std::uint8_t pin, bool output)
{
    return write_bit(kRegGpioModeLow, kRegGpioModeHigh, pin, output);
}

tl::expected<void, Error> Py32Expander::set_pull_up(std::uint8_t pin, bool enable)
{
    // Clear pull-down first to avoid pull-up/down conflict, then set pull-up.
    if (auto r = write_bit(kRegGpioPullDownLow, kRegGpioPullDownHigh, pin, false); !r) {
        return r;
    }
    return write_bit(kRegGpioPullUpLow, kRegGpioPullUpHigh, pin, enable);
}

tl::expected<void, Error> Py32Expander::digital_write(std::uint8_t pin, bool level)
{
    return write_bit(kRegGpioOutLow, kRegGpioOutHigh, pin, level);
}

tl::expected<void, Error> Py32Expander::set_led_count(std::uint8_t count)
{
    if (count > kMaxLeds) count = kMaxLeds;
    // Plain overwrite (matching upstream M5 BSP). The upper bits of REG_LED_CFG
    // aren't documented; if they actually carry mode flags the PY32 firmware
    // would reset them along with count, and we should too.
    if (!m5::In_I2C.writeRegister8(address_, kRegLedCfg,
                                   count & kLedCfgCountMask, kI2cFreq)) {
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

tl::expected<void, Error>
Py32Expander::write_led_colors(const std::uint8_t* data, std::size_t count)
{
    if (data == nullptr || count == 0) return {};
    if (count > kMaxLeds) count = kMaxLeds;
    // One burst write of count*3 bytes starting at REG_LED_RAM_START. The PY32
    // auto-increments its internal pointer, so the bytes land at slots 0..count-1.
    // Byte order per LED is GRB; see header comment.
    if (!m5::In_I2C.writeRegister(address_, kRegLedRamStart, data,
                                  count * 3, kI2cFreq)) {
        static int s_warn_throttle = 0;
        if ((s_warn_throttle++ & 31) == 0) {
            ESP_LOGW("py32", "led RAM write failed (count=%u)", (unsigned)count);
        }
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

tl::expected<void, Error> Py32Expander::refresh_leds()
{
    // Set bit 6 — the PY32 self-clears it after latching the RAM out onto the
    // NeoPixel wire. We OR it in to leave the count bits untouched.
    const std::uint8_t current = m5::In_I2C.readRegister8(address_, kRegLedCfg, kI2cFreq);
    if (!m5::In_I2C.writeRegister8(address_, kRegLedCfg,
                                   current | kLedCfgRefreshBit, kI2cFreq)) {
        static int s_warn_throttle = 0;
        if ((s_warn_throttle++ & 31) == 0) {
            ESP_LOGW("py32", "led refresh write failed");
        }
        return tl::unexpected{Error::ExpanderWrite};
    }
    return {};
}

} // namespace stackchan::board
