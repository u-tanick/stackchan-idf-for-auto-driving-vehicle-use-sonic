// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "avatar/canvas.hpp"
#include "shared_state.hpp"

namespace stackchan::app::mode_select {

void init(SharedState& state);
bool active();
void show();
void hide();
bool draw(avatar::RichCanvas& canvas);
bool handle_tap(int x, int y);

} // namespace stackchan::app::mode_select
