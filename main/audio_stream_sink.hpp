// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "shared_state.hpp"

namespace stackchan::app::audio_stream {

// Initialise the BLE audio streaming sink and register it with
// config_service. Spawns a worker task that owns the AAC decoder, drains
// incoming ADTS bytes, plays the decoded PCM through M5.Speaker, and
// updates `state.mouth_open` from the chunk RMS.
//
// Must be called after Board::begin() so M5.Speaker is initialised.
void start(SharedState& state);

} // namespace stackchan::app::audio_stream
