// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <config_service/config_service.hpp>
#include <tl/expected.hpp>

namespace stackchan::config::store {

DeviceConfig load();
tl::expected<void, Error> save(const DeviceConfig& cfg);

// Persist just the LED tuple without re-writing every other NVS key. Used
// by the BLE/HTTP LED patch sink to avoid hammering wear-levelling on the
// shared "stackchan_cfg" namespace every time a slider moves.
tl::expected<void, Error> save_led_state(std::uint8_t mode, std::uint32_t color,
                                         std::uint8_t brightness,
                                         std::uint8_t gradient_period_ds);

} // namespace stackchan::config::store
