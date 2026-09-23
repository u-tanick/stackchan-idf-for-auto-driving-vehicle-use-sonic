// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/led_strip.hpp"
#include "shared_state.hpp"

namespace stackchan::app {

struct LedTaskArgs {
    SharedState* state;
    board::LedStrip* strip; // owned by Board, null if no nekomimi strip
    board::LedStrip* base_strip{nullptr}; // Base PY32 LED strip (optional)
};

// Pinned to core 1 (off the servo / NimBLE / I2C-heavy core), ~30 Hz refresh.
// Reads led_mode / led_color / led_brightness from SharedState and pushes the
// resulting frame to the strip. No-op on boards without a strip — main only
// calls start_led_task when board.led_strip() != nullptr.
void start_led_task(LedTaskArgs& args);

} // namespace stackchan::app
