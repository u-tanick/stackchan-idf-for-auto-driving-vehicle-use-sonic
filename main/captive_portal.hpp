// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <esp_http_server.h>

namespace stackchan::app::captive_portal {

// Start a tiny UDP/53 server that resolves every A query to the SoftAP IP
// (192.168.4.1). This is what lets iOS / Android trigger their captive-portal
// detection probes (apple/connecttest etc.) — they look up a fixed host,
// hit our HTTP catch-all, and pop the captive-portal sheet. Without DNS
// hijack the probe times out and iOS refuses to mark the network as
// "connected (no internet)" reliably.
//
// Idempotent. Starts a low-priority task on first call.
void start_dns();
void stop_dns();

// Register a catch-all HTTP handler on the wifi_config server that 302s
// every unknown path to the settings page. Must be called *after*
// wifi_config::start has brought the server up and only while AP mode is
// active (the server is also used over STA, where we don't want to hijack
// arbitrary 404s).
void register_http_catchall(httpd_handle_t server);
void unregister_http_catchall(httpd_handle_t server);

} // namespace stackchan::app::captive_portal
