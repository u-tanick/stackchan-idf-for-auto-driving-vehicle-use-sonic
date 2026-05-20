// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <config_service/config_service.hpp>

namespace stackchan::app {

// Start the Wi-Fi station using credentials from cfg. If cfg.wifi_ssid is empty
// the driver is initialised but no connection is attempted; wifi_is_connected()
// will stay false and demo_loop will show the "Wi-Fi: 切断中" balloon.
//
// Returns as soon as the driver is started — connection happens asynchronously.
void wifi_start(const config::DeviceConfig& cfg);

// True once Wi-Fi has an IP address. Becomes false again on disconnect.
bool wifi_is_connected();

// Pause Wi-Fi: disassociate from the AP and suppress the event handler's
// auto-reconnect path. Use during BLE audio streaming so the radio's BT
// time isn't stolen by association maintenance. Idempotent.
void wifi_pause();

// Resume Wi-Fi: re-enable auto-reconnect and kick a connect attempt.
// No-op if not currently paused.
void wifi_resume();

} // namespace stackchan::app
