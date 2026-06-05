// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <tl/expected.hpp>

// Phase 1 surface — just enough to verify HTTPS to api.telegram.org works
// and the bot token is usable. Phase 2+ will replace this with a polling
// task + incoming-message sink + sendMessage API; see PLAN.md §8.

namespace stackchan::telegram {

enum class Error : std::uint8_t {
    InvalidToken = 1,   // empty / malformed token
    HttpInit,           // esp_http_client_init failed (heap?)
    HttpPerform,        // network / TLS / 4xx / 5xx
    BadResponse,        // JSON parse failed or `ok:false` from Telegram
    Truncated,          // response > body buffer
};

const char* to_string(Error e) noexcept;

// One-shot call to GET /bot<token>/getUpdates. Logs the parsed result count
// + the first message's text (when present) at INFO level. Blocks the
// calling task until the request completes or fails (TLS handshake +
// long-poll wait). `timeout_sec` is the Telegram-side long-poll timeout;
// the underlying esp_http_client uses timeout_sec + 5 as its hard cap so a
// stuck connection doesn't hang the task indefinitely.
//
// `token` must be the BotFather string ("<digits>:<letters>"). Pass an
// optional `offset` to acknowledge previous updates; default 0 returns all
// pending updates from the start of the queue (use the highest `update_id`
// you've seen + 1 for normal polling).
tl::expected<std::int64_t, Error>
get_updates_one_shot(std::string_view token,
                     std::int64_t offset = 0,
                     int timeout_sec = 5);

} // namespace stackchan::telegram
