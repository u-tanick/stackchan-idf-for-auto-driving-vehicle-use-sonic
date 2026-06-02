// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

class Py32Expander {
public:
    static constexpr std::uint8_t kAddress = 0x6F;
    static constexpr std::uint32_t kI2cFreq = 100'000;

    static constexpr std::uint8_t kPinServoPowerEnable = 0;

    // M5 Stack-chan base mounts 12 WS2812 NeoPixels on the back, driven by
    // the PY32 MCU on its IO14 pin and exposed via I2C as a small RAM (RGB565
    // LE per LED) + a config register that holds the count + a refresh-trigger
    // bit. We never bit-bang the NeoPixel protocol from the ESP — the PY32
    // does it.
    static constexpr std::uint8_t kMaxLeds = 32;

    static tl::expected<Py32Expander, Error> probe(std::uint8_t address = kAddress);

    tl::expected<void, Error> set_direction(std::uint8_t pin, bool output);
    tl::expected<void, Error> set_pull_up(std::uint8_t pin, bool enable);
    tl::expected<void, Error> digital_write(std::uint8_t pin, bool level);

    // Set how many of the up-to-32 LEDs are active (the rest are ignored even
    // if data is written into their slots). Writes the count to REG_LED_CFG
    // [5:0], preserving the refresh bit.
    tl::expected<void, Error> set_led_count(std::uint8_t count);

    // Write the full strip in one I2C burst as GRB byte triples (G, R, B —
    // WS2812 wire order, not RGB). `data` is count*3 bytes; the call returns
    // immediately after the I2C transaction — refresh_leds() must be called
    // separately to latch the buffer onto the strip.
    //
    // (The upstream M5 BSP driver packs as 2-byte RGB565 and writes only 2
    // bytes per LED; that turns out to be incorrect for this PY32 firmware,
    // which stores 3 bytes per LED in the order the chip will clock out onto
    // the WS2812 line. We confirmed empirically: sending {R=0xFF, G=0, B=0}
    // as RGB-ordered bytes still came out green, so byte 0 is G.)
    tl::expected<void, Error>
    write_led_colors(const std::uint8_t* data, std::size_t count);

    // Toggle the refresh bit so the PY32 latches the RAM contents out onto
    // the NeoPixel wire. set_led_count / write_led_colors do not auto-refresh.
    tl::expected<void, Error> refresh_leds();

    std::uint8_t address() const noexcept { return address_; }

private:
    explicit Py32Expander(std::uint8_t address) noexcept : address_{address} {}

    tl::expected<void, Error> write_bit(std::uint8_t reg_l, std::uint8_t reg_h, std::uint8_t pin, bool value);

    std::uint8_t address_;
};

} // namespace stackchan::board
