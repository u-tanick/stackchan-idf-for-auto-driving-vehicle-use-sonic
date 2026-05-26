// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "device_ui.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>

#include <config_service/config_service.hpp>
#include <config_service/config_store.hpp>

#include "wifi_sta.hpp"

namespace stackchan::app::ui {

namespace {

constexpr int kW = 320;
constexpr int kH = 240;

// Top bar: tabs + a close button on the right. When there are more tabs than
// fit (kTabsPerPage), the bar paginates with ‹ › arrows.
constexpr int kBarH = 38;
constexpr int kCloseW = 38;
constexpr int kArrowW = 26;
constexpr int kTabsPerPage = 3;

// Content rows.
constexpr int kContentY = kBarH + 8;     // 46
constexpr int kRowH = 42;

enum Page : int { kInfo = 0, kSettings = 1, kControl = 2, kConversation = 3, kTabCount };
const char* const kTabLabels[kTabCount] = {"情報", "設定", "操作", "会話"};
int num_tab_pages() { return (kTabCount + kTabsPerPage - 1) / kTabsPerPage; }

const auto* const kFontTitle = &fonts::lgfxJapanGothic_24;
const auto* const kFontBody = &fonts::lgfxJapanGothic_16;

SharedState* g_state = nullptr;

std::atomic<bool> g_active{false};
std::atomic<int> g_page{kInfo};
std::atomic<int> g_tab_page{0}; // which group of tabs is visible (paging)
std::atomic<bool> g_dirty{true};
int g_provider = 0; // 0 = OpenAI, 1 = Gemini, 2 = XiaoZhi (cached at init)

// Staged settings (loaded from NVS on open; applied on 適用).
std::atomic<bool> g_stage_conv{true};
std::atomic<bool> g_stage_rtp{true};

// Cached once at init() (don't change at runtime) — read by the render task.
std::string g_ssid;
std::string g_host;

M5Canvas g_canvas; // standalone sprite, pushed with an explicit target
bool g_canvas_ready = false;
std::uint32_t g_last_info_ms = 0;

std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

// Row hit/draw rectangle (full-width, indented).
void row_rect(int i, int& rx, int& ry, int& rw, int& rh)
{
    rx = 10;
    ry = kContentY + i * kRowH;
    rw = kW - 20;
    rh = kRowH - 6;
}

std::string current_ip()
{
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info{};
    if (nif != nullptr && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr != 0) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
        return std::string(buf);
    }
    return "-";
}

// --- Tab bar layout (shared by draw + hit-test) --------------------------

struct TabSlot {
    int index; // tab index (Page)
    int x;
    int w;
};
struct TabBar {
    int close_x;
    bool paging = false;
    int prev_x = 0, next_x = 0; // arrow x (width kArrowW) when paging
    TabSlot slots[kTabsPerPage];
    int slot_count = 0;
};

TabBar layout_tabbar()
{
    TabBar b;
    b.close_x = kW - kCloseW;
    if (kTabCount <= kTabsPerPage) {
        const int avail = kW - kCloseW;
        const int tw = avail / kTabCount;
        for (int i = 0; i < kTabCount; ++i) {
            const int x = i * tw;
            const int w = (i == kTabCount - 1) ? (avail - tw * (kTabCount - 1)) : tw;
            b.slots[b.slot_count++] = {i, x, w};
        }
    } else {
        b.paging = true;
        b.prev_x = 0;
        b.next_x = kW - kCloseW - kArrowW;
        const int sx = kArrowW;
        const int sw = b.next_x - sx;
        const int tw = sw / kTabsPerPage;
        const int np = num_tab_pages();
        const int start = (g_tab_page.load(std::memory_order_relaxed) % np) * kTabsPerPage;
        for (int s = 0; s < kTabsPerPage && (start + s) < kTabCount; ++s) {
            b.slots[b.slot_count++] = {start + s, sx + s * tw, tw};
        }
    }
    return b;
}

// --- Drawing -------------------------------------------------------------

void draw_topbar(int page)
{
    const std::uint16_t bar = g_canvas.color565(40, 44, 54);
    const std::uint16_t sel = g_canvas.color565(60, 120, 200);
    const std::uint16_t fg = g_canvas.color565(235, 235, 235);
    const std::uint16_t arrow = g_canvas.color565(70, 74, 84);
    g_canvas.fillRect(0, 0, kW, kBarH, bar);

    const TabBar b = layout_tabbar();
    g_canvas.setFont(kFontBody);
    g_canvas.setTextDatum(lgfx::textdatum_t::middle_center);

    if (b.paging) {
        g_canvas.fillRect(b.prev_x, 0, kArrowW, kBarH, arrow);
        g_canvas.fillRect(b.next_x, 0, kArrowW, kBarH, arrow);
        // Draw the paging arrows as triangles (font has no ‹ › glyphs).
        const int cy = kBarH / 2;
        const int pcx = b.prev_x + kArrowW / 2;
        g_canvas.fillTriangle(pcx - 5, cy, pcx + 4, cy - 7, pcx + 4, cy + 7, fg); // ◀
        const int ncx = b.next_x + kArrowW / 2;
        g_canvas.fillTriangle(ncx + 5, cy, ncx - 4, cy - 7, ncx - 4, cy + 7, fg); // ▶
    }
    for (int i = 0; i < b.slot_count; ++i) {
        const TabSlot& s = b.slots[i];
        if (s.index == page) g_canvas.fillRect(s.x, 0, s.w, kBarH, sel);
        g_canvas.setTextColor(fg);
        g_canvas.drawString(kTabLabels[s.index], s.x + s.w / 2, kBarH / 2);
    }
    // Close button.
    g_canvas.fillRect(b.close_x, 0, kCloseW, kBarH, g_canvas.color565(120, 60, 60));
    g_canvas.setTextColor(fg);
    g_canvas.drawString("×", b.close_x + kCloseW / 2, kBarH / 2);
}

void draw_kv(int y, const char* key, const char* value, std::uint16_t vcolor)
{
    const std::uint16_t dim = g_canvas.color565(150, 150, 150);
    g_canvas.setFont(kFontBody);
    g_canvas.setTextDatum(lgfx::textdatum_t::top_left);
    g_canvas.setTextColor(dim);
    g_canvas.drawString(key, 12, y);
    g_canvas.setTextColor(vcolor);
    g_canvas.drawString(value, 120, y);
}

void draw_info()
{
    const std::uint16_t fg = g_canvas.color565(235, 235, 235);
    const std::uint16_t ok = g_canvas.color565(80, 220, 120);
    const std::uint16_t off = g_canvas.color565(230, 110, 110);
    const std::uint16_t warn = g_canvas.color565(235, 200, 90);
    const std::uint16_t dim = g_canvas.color565(150, 150, 150);

    const esp_app_desc_t* app = esp_app_get_description();
    const bool wifi = wifi_is_connected();
    const bool ble = config::ble_connected();
    const std::string ip = current_ip();
    const std::uint32_t up = now_ms() / 1000;
    char uptime[24];
    if (up >= 3600) {
        std::snprintf(uptime, sizeof(uptime), "%uh%02um%02us", static_cast<unsigned>(up / 3600),
                      static_cast<unsigned>((up % 3600) / 60), static_cast<unsigned>(up % 60));
    } else {
        std::snprintf(uptime, sizeof(uptime), "%um%02us", static_cast<unsigned>(up / 60),
                      static_cast<unsigned>(up % 60));
    }
    char heap[16];
    std::snprintf(heap, sizeof(heap), "%u KB", static_cast<unsigned>(esp_get_free_heap_size() / 1024));

    // Battery (INA226, refreshed by demo_loop). pct < 0 → not yet read / absent.
    const int bat_mv = g_state->battery_mv.load(std::memory_order_relaxed);
    const int bat_ma = g_state->battery_ma.load(std::memory_order_relaxed);
    const int bat_pct = g_state->battery_pct.load(std::memory_order_relaxed);
    char battery[32];
    std::uint16_t bat_color = dim;
    if (bat_pct < 0 || bat_mv < 0) {
        std::snprintf(battery, sizeof(battery), "—");
    } else {
        std::snprintf(battery, sizeof(battery), "%d%% %.2fV %dmA", bat_pct, bat_mv / 1000.0f, bat_ma);
        bat_color = bat_pct >= 50 ? ok : (bat_pct >= 20 ? warn : off);
    }

    int y = kContentY;
    const int dy = 22; // 9 rows must fit kContentY(46)..kH(240)
    draw_kv(y, "FW", app ? app->version : "?", fg); y += dy;
    draw_kv(y, "SSID", g_ssid.empty() ? "(未設定)" : g_ssid.c_str(), fg); y += dy;
    draw_kv(y, "mDNS", g_host.c_str(), fg); y += dy;
    draw_kv(y, "IP", ip.c_str(), wifi ? fg : off); y += dy;
    draw_kv(y, "Wi-Fi", wifi ? "接続中" : "未接続", wifi ? ok : off); y += dy;
    draw_kv(y, "BLE", ble ? "接続中" : "待受中", ble ? ok : fg); y += dy;
    draw_kv(y, "稼働", uptime, fg); y += dy;
    draw_kv(y, "空きRAM", heap, fg); y += dy;
    draw_kv(y, "電池", battery, bat_color); y += dy;
}

void draw_toggle_row(int i, const char* label, bool on)
{
    int rx, ry, rw, rh;
    row_rect(i, rx, ry, rw, rh);
    g_canvas.fillRoundRect(rx, ry, rw, rh, 6, g_canvas.color565(40, 44, 54));
    g_canvas.setFont(kFontBody);
    g_canvas.setTextDatum(lgfx::textdatum_t::middle_left);
    g_canvas.setTextColor(g_canvas.color565(235, 235, 235));
    g_canvas.drawString(label, rx + 12, ry + rh / 2);
    // Pill switch on the right.
    const int pw = 58, ph = 26;
    const int px = rx + rw - pw - 10, py = ry + (rh - ph) / 2;
    const std::uint16_t on_c = g_canvas.color565(80, 200, 120);
    const std::uint16_t off_c = g_canvas.color565(90, 90, 96);
    g_canvas.fillRoundRect(px, py, pw, ph, ph / 2, on ? on_c : off_c);
    const int kn = ph - 6;
    g_canvas.fillCircle(on ? (px + pw - ph / 2) : (px + ph / 2), py + ph / 2, kn / 2,
                        g_canvas.color565(245, 245, 245));
}

void draw_button(int i, const char* label, std::uint16_t color)
{
    int rx, ry, rw, rh;
    row_rect(i, rx, ry, rw, rh);
    g_canvas.fillRoundRect(rx, ry, rw, rh, 6, color);
    g_canvas.setFont(kFontBody);
    g_canvas.setTextDatum(lgfx::textdatum_t::middle_center);
    g_canvas.setTextColor(g_canvas.color565(245, 245, 245));
    g_canvas.drawString(label, rx + rw / 2, ry + rh / 2);
}

void draw_settings()
{
    draw_toggle_row(0, "会話機能", g_stage_conv.load(std::memory_order_relaxed));
    draw_toggle_row(1, "RTP 音声受信", g_stage_rtp.load(std::memory_order_relaxed));
    draw_button(3, "適用（保存して再起動）", g_canvas.color565(60, 120, 200));
    g_canvas.setFont(kFontBody);
    g_canvas.setTextDatum(lgfx::textdatum_t::top_left);
    g_canvas.setTextColor(g_canvas.color565(150, 150, 150));
    g_canvas.drawString("変更は適用で反映されます", 12, kContentY + 2 * kRowH + 2);
}

void draw_control()
{
    const bool servo_on = g_state->servo_enabled.load(std::memory_order_relaxed);
    draw_toggle_row(0, "サーボ（脱力/復帰）", servo_on);
    draw_button(1, "姿勢をリセット", g_canvas.color565(60, 120, 200));
}

void draw_conversation()
{
    const std::uint16_t fg = g_canvas.color565(235, 235, 235);
    const std::uint16_t ok = g_canvas.color565(80, 220, 120);
    const std::uint16_t warn = g_canvas.color565(235, 200, 90);
    const std::uint16_t err = g_canvas.color565(235, 100, 100);
    const std::uint16_t dim = g_canvas.color565(150, 150, 150);

    const ConvStatus st = g_state->conversation_status.load(std::memory_order_relaxed);
    const char* status_text = "?";
    std::uint16_t status_color = dim;
    switch (st) {
    case ConvStatus::Disabled: status_text = "無効"; status_color = dim; break;
    case ConvStatus::WaitingWifi: status_text = "Wi-Fi 待ち"; status_color = warn; break;
    case ConvStatus::Connecting: status_text = "接続中…"; status_color = warn; break;
    case ConvStatus::Listening: status_text = "接続（待受）"; status_color = ok; break;
    case ConvStatus::Talking: status_text = "通話中"; status_color = ok; break;
    case ConvStatus::Yielded: status_text = "音声再生中"; status_color = dim; break;
    case ConvStatus::Reconnecting: status_text = "再接続中…"; status_color = warn; break;
    case ConvStatus::Error: status_text = "接続エラー"; status_color = err; break;
    }
    const std::uint32_t reconnects = g_state->conversation_reconnects.load(std::memory_order_relaxed);
    char rc[16];
    std::snprintf(rc, sizeof(rc), "%u 回", static_cast<unsigned>(reconnects));

    int y = kContentY;
    const int dy = 26;
    const char* provider_name = g_provider == 1   ? "Gemini Live"
                                : g_provider == 2 ? "XiaoZhi"
                                                  : "OpenAI Realtime";
    draw_kv(y, "サービス", provider_name, fg); y += dy;
    draw_kv(y, "状態", status_text, status_color); y += dy;
    draw_kv(y, "再接続", rc, reconnects > 0 ? warn : fg); y += dy;
}

void render_page(M5GFX& display)
{
    const int page = g_page.load(std::memory_order_relaxed);
    g_canvas.fillScreen(g_canvas.color565(20, 22, 28));
    draw_topbar(page);
    if (page == kInfo) {
        draw_info();
    } else if (page == kSettings) {
        draw_settings();
    } else if (page == kControl) {
        draw_control();
    } else {
        draw_conversation();
    }
    g_canvas.pushSprite(&display, 0, 0);
}

// --- Actions -------------------------------------------------------------

void load_staged()
{
    const config::DeviceConfig cfg = config::load();
    g_stage_conv.store(cfg.openai_enabled, std::memory_order_relaxed);
    g_stage_rtp.store(cfg.rtp_audio_enabled, std::memory_order_relaxed);
}

void apply_and_reboot()
{
    config::DeviceConfig cfg = config::load();
    cfg.openai_enabled = g_stage_conv.load(std::memory_order_relaxed);
    cfg.rtp_audio_enabled = g_stage_rtp.load(std::memory_order_relaxed);
    (void)config::store::save(cfg);
    esp_restart();
}

} // namespace

void init(SharedState& state)
{
    g_state = &state;
    const config::DeviceConfig cfg = config::load();
    g_ssid = cfg.wifi_ssid;
    g_provider = static_cast<int>(cfg.provider);
    g_stage_conv.store(cfg.openai_enabled, std::memory_order_relaxed);
    g_stage_rtp.store(cfg.rtp_audio_enabled, std::memory_order_relaxed);
    std::uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char host[32];
    std::snprintf(host, sizeof(host), "stackchan-%02x%02x%02x.local", mac[3], mac[4], mac[5]);
    g_host = host;
}

bool active()
{
    return g_active.load(std::memory_order_relaxed);
}

void handle_tap(int x, int y)
{
    if (!g_active.load(std::memory_order_relaxed)) {
        // Avatar showing: a tap in the top-right corner opens the UI.
        if (x >= kW - 64 && y < 64) {
            load_staged();
            g_page.store(kInfo, std::memory_order_relaxed);
            g_tab_page.store(0, std::memory_order_relaxed); // 情報 is on page 0
            g_active.store(true, std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        }
        return;
    }

    // Top bar.
    if (y < kBarH) {
        const TabBar b = layout_tabbar();
        if (x >= b.close_x) { // close
            g_active.store(false, std::memory_order_relaxed);
            return;
        }
        const int np = num_tab_pages();
        if (b.paging && x >= b.prev_x && x < b.prev_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + np - 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return;
        }
        if (b.paging && x >= b.next_x && x < b.next_x + kArrowW) {
            g_tab_page.store((g_tab_page.load(std::memory_order_relaxed) + 1) % np,
                             std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
            return;
        }
        for (int i = 0; i < b.slot_count; ++i) {
            if (x >= b.slots[i].x && x < b.slots[i].x + b.slots[i].w) {
                g_page.store(b.slots[i].index, std::memory_order_relaxed);
                g_dirty.store(true, std::memory_order_relaxed);
                break;
            }
        }
        return;
    }

    // Content, per page.
    const int page = g_page.load(std::memory_order_relaxed);
    auto hit_row = [&](int i) {
        int rx, ry, rw, rh;
        row_rect(i, rx, ry, rw, rh);
        return in_rect(x, y, rx, ry, rw, rh);
    };
    if (page == kSettings) {
        if (hit_row(0)) {
            g_stage_conv.store(!g_stage_conv.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_stage_rtp.store(!g_stage_rtp.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(3)) {
            apply_and_reboot(); // does not return
        }
    } else if (page == kControl) {
        if (hit_row(0)) {
            g_state->servo_enabled.store(!g_state->servo_enabled.load(std::memory_order_relaxed),
                                         std::memory_order_relaxed);
            g_dirty.store(true, std::memory_order_relaxed);
        } else if (hit_row(1)) {
            g_state->target_yaw_deg.store(0.0f, std::memory_order_relaxed);
            g_state->target_pitch_deg.store(0.0f, std::memory_order_relaxed);
        }
    }
}

void draw(M5GFX& display)
{
    if (!g_canvas_ready) {
        g_canvas.setColorDepth(16);
        g_canvas.setPsram(true);
        if (g_canvas.createSprite(kW, kH) == nullptr) return; // retry next frame
        g_canvas_ready = true;
    }

    bool need = g_dirty.exchange(false, std::memory_order_relaxed);
    // The info and 会話 pages have live fields (IP/uptime/heap, conn status /
    // reconnect count) — refresh ~2 Hz so they stay current.
    const int page = g_page.load(std::memory_order_relaxed);
    if (page == kInfo || page == kConversation) {
        const std::uint32_t t = now_ms();
        if (t - g_last_info_ms > 500) {
            g_last_info_ms = t;
            need = true;
        }
    }
    if (need) render_page(display);
}

} // namespace stackchan::app::ui
