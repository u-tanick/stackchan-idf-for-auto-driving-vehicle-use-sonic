// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5GFX.h>

#include "shared_state.hpp"

namespace stackchan::app {

struct RenderTaskArgs {
    M5GFX* display;
    SharedState* state;
    // True when the underlying display is round (M5 StopWatch's CO5300 AMOLED).
    // Render task forwards the hint to the avatar Canvas so the VM scales the
    // 320×240 design space down into the inscribed square instead of stretching
    // features outside the visible circle.
    bool circular_display = false;
};

// Pinned to core 1, 33 ms period.
void start_render_task(RenderTaskArgs& args);

} // namespace stackchan::app
