// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "board/si12t_touch.hpp"
#include "config_service/config_service.hpp"
#include "shared_state.hpp"

namespace stackchan::app {

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
};

// Pinned to core 0. Owns the chosen ConversationService backend and drives
// the half-duplex mic/speaker I2S state machine, avatar mouth sync, balloon
// text, and tool dispatch. Waits for Wi-Fi before connecting.
void start_conversation_task(ConversationTaskArgs& args);

} // namespace stackchan::app
