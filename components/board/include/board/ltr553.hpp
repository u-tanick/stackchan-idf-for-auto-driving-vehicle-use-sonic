// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <optional>
#include <tl/expected.hpp>

#include "board/board.hpp"

namespace stackchan::board {

// CoreS3 onboard ambient light and proximity sensor (LTR-553ALS-WA, I2C 0x23 on internal bus).
// Measures proximity (PS) with 11-bit resolution (0..2047) to detect obstacles
// right in front of the CoreS3 face within a few centimeters to ~10 cm.
class Ltr553Proximity {
public:
    static constexpr std::uint8_t kAddress = 0x23;
    static constexpr std::uint32_t kI2cFreq = 100'000;

    // Registers
    static constexpr std::uint8_t kRegAlsContr   = 0x80;
    static constexpr std::uint8_t kRegPsContr    = 0x81;
    static constexpr std::uint8_t kRegPsLed      = 0x82;
    static constexpr std::uint8_t kRegPsNPulses  = 0x83;
    static constexpr std::uint8_t kRegPsMeasRate = 0x84;
    static constexpr std::uint8_t kRegAlsPsStatus= 0x8C;
    static constexpr std::uint8_t kRegPsData0    = 0x8D;
    static constexpr std::uint8_t kRegPsData1    = 0x8E;

    // Default threshold for close obstacle detection
    // Typical ambient/clear reading is < 50; objects within ~5-10cm yield > 200..2047.
    static constexpr std::uint16_t kDefaultObstacleThreshold = 250;

    struct Reading {
        std::uint16_t ps_raw{0};        // 0..2047 (11-bit ADC)
        bool saturated{false};          // PS sensor saturated flag
        bool obstacle_near{false};      // ps_raw >= threshold
    };

    // Probe the sensor at 0x23 on internal I2C bus and initialize PS mode
    static std::optional<Ltr553Proximity> probe();

    // Static convenience helper: lazily initializes and reads proximity
    static std::optional<Reading> read(std::uint16_t threshold = kDefaultObstacleThreshold);

    // Read proximity value (samples multiple times for stability)
    std::optional<Reading> read_proximity(std::uint16_t threshold = kDefaultObstacleThreshold);

    // Single-shot raw read without multi-sampling
    std::optional<Reading> read_raw(std::uint16_t threshold = kDefaultObstacleThreshold);

private:
    Ltr553Proximity() = default;
    static bool write_reg(std::uint8_t reg, std::uint8_t val);
    static bool read_regs(std::uint8_t reg, std::uint8_t* buf, std::size_t len);
};

} // namespace stackchan::board
