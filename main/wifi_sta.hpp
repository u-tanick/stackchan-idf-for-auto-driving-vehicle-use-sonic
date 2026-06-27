// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>

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

// --- SoftAP provisioning ---
// Switch Wi-Fi into APSTA so iOS (no Web Bluetooth) can join the device's own
// AP and use the existing wifi_config HTTP settings page to configure home
// Wi-Fi. STA stays attempting reconnect, so a successful STA association does
// not tear the AP down — the user can keep editing settings until they tap
// "AP mode" again on the device UI.
//
// Enable also (idempotently) starts wifi_config_service, flips it into
// provisioning mode (require_auth bypass + provisioning_mode flag in
// /api/status), and brings up the captive portal (DNS hijack + 404 catch-all
// pointing to the settings page).
//   - SSID = "Stackchan-XXXXXX" (last 3 STA MAC bytes — same as BLE / mDNS).
//   - WPA2 PSK = "sc-XXXXXXXX"   (last 4 STA MAC bytes, lower hex).
// Both idempotent.
void wifi_enable_ap_mode();
void wifi_disable_ap_mode();
bool wifi_ap_active();

// AP join info for the on-device QR screen. Returns false if the AP isn't up
// or the buffers are too small.
bool wifi_ap_info(char* ssid, std::size_t ssid_cap,
                  char* password, std::size_t pw_cap,
                  char* ip, std::size_t ip_cap);

} // namespace stackchan::app
