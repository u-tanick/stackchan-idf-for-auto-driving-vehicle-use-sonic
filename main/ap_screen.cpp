// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "ap_screen.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "wifi_sta.hpp"

namespace stackchan::app::ap_screen {

namespace {

// Track the last (SSID, PW, IP) tuple painted so we only redraw when the
// driver flips state. The QR encode + multi-rect fillRect is expensive on
// large round panels (StopWatch 466×466) and we don't want to burn it every
// frame at 30 fps.
char g_last_ssid[33] = {0};
char g_last_pw[33]   = {0};
char g_last_ip[16]   = {0};
bool g_painted = false;

// Build the standard Wi-Fi QR payload. Format documented by the Wi-Fi
// Alliance and supported natively by iOS Camera / Android scanners:
//   WIFI:T:<auth>;S:<SSID>;P:<password>;H:<hidden>;;
// Special chars `\`, `;`, `,`, `"`, `:` must be escaped with `\` inside SSID
// and password — our derived SSID/PW use only [A-Za-z0-9-], so no escaping
// is actually needed; we still apply it for safety should the credential
// derivation ever change.
std::size_t append_escaped(char* dst, std::size_t cap, std::size_t pos, const char* src)
{
    for (; *src != '\0' && pos + 2 < cap; ++src) {
        const char c = *src;
        if (c == '\\' || c == ';' || c == ',' || c == '"' || c == ':') {
            dst[pos++] = '\\';
        }
        dst[pos++] = c;
    }
    dst[pos] = '\0';
    return pos;
}

void build_wifi_qr_payload(const char* ssid, const char* pw, char* out, std::size_t cap)
{
    std::size_t p = 0;
    constexpr const char* kPrefix = "WIFI:T:WPA;S:";
    constexpr const char* kMid    = ";P:";
    constexpr const char* kSuffix = ";H:false;;";
    p += std::snprintf(out + p, cap - p, "%s", kPrefix);
    p = append_escaped(out, cap, p, ssid);
    p += std::snprintf(out + p, cap - p, "%s", kMid);
    p = append_escaped(out, cap, p, pw);
    std::snprintf(out + p, cap - p, "%s", kSuffix);
}

} // namespace

bool active()
{
    return wifi_ap_active();
}

bool draw(M5GFX& display)
{
    char ssid[33], pw[33], ip[16];
    if (!wifi_ap_info(ssid, sizeof(ssid), pw, sizeof(pw), ip, sizeof(ip))) {
        return false;
    }
    // Only repaint when the credentials actually changed — saves the full
    // QR render cost on every render-task tick.
    if (g_painted &&
        std::strcmp(ssid, g_last_ssid) == 0 &&
        std::strcmp(pw, g_last_pw) == 0 &&
        std::strcmp(ip, g_last_ip) == 0) {
        return false;
    }
    std::strncpy(g_last_ssid, ssid, sizeof(g_last_ssid) - 1);
    std::strncpy(g_last_pw, pw, sizeof(g_last_pw) - 1);
    std::strncpy(g_last_ip, ip, sizeof(g_last_ip) - 1);

    const std::int32_t W = display.width();
    const std::int32_t H = display.height();

    // Layout breakpoints. Small atom panels (128×128) only have room for the
    // QR + a single line of text; mid panels (CoreS3 320×240) fit QR + SSID
    // + PW + URL stacked; large round panels (StopWatch 466×466) use a 240
    // px QR inscribed inside the visible circle with text in the corners
    // pulled in by the 70% safe inset (sqrt(2)/2).
    const bool tiny  = W <= 160;
    const bool round_ = W >= 400;

    // Pull the safe-area inset on round displays so text doesn't fall off
    // the visible circle. Square panels use a small uniform margin.
    const int inset = round_
        ? static_cast<int>(W * (1.0f - 0.70710678f) * 0.5f) + 4
        : 4;

    const int qr_w = tiny ? (W - 18)
                   : round_ ? (W * 240 / 466)
                   : 160;
    const int qr_x = (W - qr_w) / 2;
    const int qr_y = tiny ? 8
                   : round_ ? (H / 2 - qr_w / 2)
                   : (inset + 26); // leave room for the title line on CoreS3

    char qr_payload[160];
    build_wifi_qr_payload(ssid, pw, qr_payload, sizeof(qr_payload));

    display.startWrite();
    display.fillScreen(TFT_WHITE);

    // Title — skipped on the tiny panel (no space).
    if (!tiny) {
        display.setFont(round_ ? &fonts::lgfxJapanGothic_24
                                : &fonts::lgfxJapanGothic_16);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        const int title_y = round_ ? (qr_y - 32) : inset;
        display.drawString("Wi-Fi 設定モード", W / 2, title_y);
    }

    // QR. Version 0 = auto-pick by payload length; margin=true adds the
    // standard quiet zone the M5GFX implementation expects for reliable
    // decoding. The payload is ≤ 80 chars in practice, well within the
    // capacity of an auto-sized QR at the default ECC level.
    display.qrcode(qr_payload, qr_x, qr_y, qr_w, 0);

    // Caption block below the QR — SSID / PW / URL. Use a smaller font on
    // the tiny panel; otherwise share the title font scale.
    if (!tiny) {
        display.setFont(round_ ? &fonts::lgfxJapanGothic_20
                                : &fonts::lgfxJapanGothic_12);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        const int line_h = round_ ? 28 : 16;
        int ty = qr_y + qr_w + (round_ ? 12 : 6);
        char buf[64];

        std::snprintf(buf, sizeof(buf), "SSID: %s", ssid);
        display.drawString(buf, W / 2, ty);
        ty += line_h;

        std::snprintf(buf, sizeof(buf), "PW:   %s", pw);
        display.drawString(buf, W / 2, ty);
        ty += line_h;

        std::snprintf(buf, sizeof(buf), "http://%s/", ip);
        display.drawString(buf, W / 2, ty);
    } else {
        // 128×128: only the SSID fits readably under the QR. iOS-camera-
        // scan-then-tap-join carries the URL via the captive portal probe,
        // so the URL line isn't critical here.
        display.setFont(&fonts::Font0);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(lgfx::textdatum_t::top_center);
        display.drawString(ssid, W / 2, qr_y + qr_w + 2);
    }
    display.endWrite();

    g_painted = true;
    return true;
}

} // namespace stackchan::app::ap_screen
