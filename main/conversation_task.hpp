// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "board/si12t_touch.hpp"
#include "config_service/config_service.hpp"
#include "shared_state.hpp"

namespace stackchan::app {

// Speaker playback ring size. Exposed here so app_main can pre-allocate the
// buffers in internal RAM during the boot-time low-fragmentation window
// (see app_main.cpp for rationale). conversation_task.cpp uses these same
// constants when reading from the ring.
constexpr std::size_t kConversationSegmentSamples = 4096;  // ~512 ms per segment at 8 kHz
constexpr std::size_t kConversationSegmentBuffers = 3;

struct ConversationTaskArgs {
    SharedState* state;
    const char* api_key;          // API key for the chosen provider; empty disables the task
    config::Provider provider;    // selects OpenAI Realtime / Gemini Live / XiaoZhi
    board::Si12tTouch* touch;     // top touch sensor for barge-in; may be null
    // XiaoZhi only: WebSocket endpoint + optional bearer token. For XiaoZhi an
    // empty url (not api_key) disables the task.
    const char* xiaozhi_url = "";
    const char* xiaozhi_token = "";
    // Conversation system prompt / persona (OpenAI / Gemini). Empty → built-in
    // default. Ignored by XiaoZhi (its server owns the persona).
    const char* system_prompt = "";
    // Extra HTTP headers for the conversation WebSocket upgrade (e.g. a
    // Cloudflare Access service token). Newline-separated "Name: value" lines.
    const char* extra_headers = "";
    // Internal-RAM playback ring, pre-allocated by app_main in the boot-time
    // low-fragmentation window. We can't allocate this lazily from the
    // conversation task: by the time it runs (post Wi-Fi association, ~12 s
    // into boot), other subsystems (camera link, mbedtls sessions, BLE
    // pools, …) have nibbled the internal-RAM pool down to <8 KiB largest
    // contiguous block and a 3 × 8 KiB alloc fails. So app_main reserves
    // the buffers right after Board::begin() (when largest ≈ 29 KiB) and
    // hands ownership in via this field. Any nullptr entry disables the
    // conversation task. Lifetime is app_main's — conv-task never frees.
    std::array<std::int16_t*, kConversationSegmentBuffers> seg_buf{};
};

// Pinned to core 0. Owns the chosen ConversationService backend and drives
// the half-duplex mic/speaker I2S state machine, avatar mouth sync, balloon
// text, and tool dispatch. Waits for Wi-Fi before connecting.
void start_conversation_task(ConversationTaskArgs& args);

} // namespace stackchan::app
