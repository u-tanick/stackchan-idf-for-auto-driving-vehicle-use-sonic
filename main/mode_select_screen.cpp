// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "mode_select_screen.hpp"
#include "atomic_motion_client.hpp"
#include "wifi_sta.hpp"
#include <cstdio>
#include <esp_log.h>

namespace stackchan::app::mode_select {

namespace {

constexpr const char* kTag = "mode_select";

SharedState* g_state = nullptr;
bool g_active = false;

const auto* const kFontTitle = &fonts::lgfxJapanGothic_16;
const auto* const kFontDesc  = &fonts::lgfxJapanGothic_12;

uint32_t s_disabled_alert_until_ms = 0;

} // namespace

void init(SharedState& state)
{
    g_state = &state;
    g_active = false;
}

bool active()
{
    return g_active;
}

void show()
{
    ESP_LOGI(kTag, "Showing mode select screen");
    g_active = true;
}

void hide()
{
    ESP_LOGI(kTag, "Hiding mode select screen");
    g_active = false;
}

bool draw(avatar::RichCanvas& canvas)
{
    if (!g_active) return false;

    // CoreS3 320x240 全画面クリア
    const uint16_t bg_dark = canvas.color565(10, 14, 20);
    canvas.fillScreen(bg_dark);

    // 縦3分割カードの共通設定（フッター領域確保のため H=68）
    constexpr int32_t kCardX = 6;
    constexpr int32_t kCardW = 308;
    constexpr int32_t kCardH = 68;
    constexpr int32_t kRadius = 8;

    // バッジ共通設定（3つすべて同一サイズ、VLM AIが余裕をもって収まる幅56px）
    constexpr int32_t kBadgeX = kCardX + 10;
    constexpr int32_t kBadgeYOff = 11;
    constexpr int32_t kBadgeW = 56;
    constexpr int32_t kBadgeH = 20;
    constexpr int32_t kBadgeRadius = 4;
    constexpr int32_t kBadgeCenterX = kBadgeX + kBadgeW / 2;
    constexpr int32_t kBadgeCenterYOff = kBadgeYOff + kBadgeH / 2;

    // タイトルおよび説明文の共通配置
    constexpr int32_t kTitleX = kBadgeX + kBadgeW + 8; // kCardX + 74
    constexpr int32_t kDescX = kCardX + 12;
    constexpr int32_t kDescYOff = 43;

    // --- 上段: 自律運転（距離センサー） ---
    {
        constexpr int32_t kCardY = 6;
        const uint16_t card_bg = canvas.color565(12, 34, 50);
        const uint16_t border_color = canvas.color565(0, 220, 255); // Cyan
        const uint16_t sub_color  = canvas.color565(140, 200, 230);

        canvas.fillRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, card_bg);
        canvas.drawRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, border_color);

        // バッジ
        canvas.fillRoundRect(kBadgeX, kCardY + kBadgeYOff, kBadgeW, kBadgeH, kBadgeRadius, border_color);
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(0, 0, 0));
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString("SONIC", kBadgeCenterX, kCardY + kBadgeCenterYOff);

        // タイトル
        canvas.setFont(kFontTitle);
        canvas.setTextColor(border_color);
        canvas.setTextDatum(lgfx::textdatum_t::middle_left);
        canvas.drawString("自律運転 (距離センサー)", kTitleX, kCardY + kBadgeCenterYOff);

        // 説明文
        canvas.setFont(kFontDesc);
        canvas.setTextColor(sub_color);
        canvas.setTextDatum(lgfx::textdatum_t::top_left);
        canvas.drawString("カメラ不使用 / 壁検知で自動左90°旋回", kDescX, kCardY + kDescYOff);
    }

    // --- 中段: 自律運転（距離＋カメラ） ---
    {
        constexpr int32_t kCardY = 80;
        const bool wifi_ok = wifi_is_connected();

        const uint16_t card_bg = wifi_ok ? canvas.color565(48, 28, 12) : canvas.color565(22, 22, 24);
        const uint16_t border_color = wifi_ok ? canvas.color565(255, 150, 0) : canvas.color565(70, 70, 75); // Orange or Dark Gray
        const uint16_t title_color  = wifi_ok ? border_color : canvas.color565(120, 120, 125);
        const uint16_t sub_color    = wifi_ok ? canvas.color565(240, 190, 140) : canvas.color565(210, 100, 100);
        const uint16_t badge_bg     = wifi_ok ? border_color : canvas.color565(50, 50, 55);
        const uint16_t badge_text   = wifi_ok ? canvas.color565(0, 0, 0) : canvas.color565(140, 140, 145);

        canvas.fillRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, card_bg);
        canvas.drawRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, border_color);

        // バッジ
        canvas.fillRoundRect(kBadgeX, kCardY + kBadgeYOff, kBadgeW, kBadgeH, kBadgeRadius, badge_bg);
        canvas.setFont(kFontDesc);
        canvas.setTextColor(badge_text);
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString(wifi_ok ? "VLM AI" : "OFFLINE", kBadgeCenterX, kCardY + kBadgeCenterYOff);

        // タイトル
        canvas.setFont(kFontTitle);
        canvas.setTextColor(title_color);
        canvas.setTextDatum(lgfx::textdatum_t::middle_left);
        canvas.drawString("自律運転 (距離＋カメラ)", kTitleX, kCardY + kBadgeCenterYOff);

        // 説明文
        canvas.setFont(kFontDesc);
        canvas.setTextColor(sub_color);
        canvas.setTextDatum(lgfx::textdatum_t::top_left);
        if (wifi_ok) {
            canvas.drawString("AI画像認識で4方向探索・最適ルート決定", kDescX, kCardY + kDescYOff);
        } else {
            canvas.drawString("※Wi-Fi未接続のため選択できません", kDescX, kCardY + kDescYOff);
        }
    }

    // --- 下段: JoyC操作（ESPNow） ---
    {
        constexpr int32_t kCardY = 154;
        const uint16_t card_bg = canvas.color565(12, 42, 22);
        const uint16_t border_color = canvas.color565(50, 220, 80); // Green
        const uint16_t sub_color  = canvas.color565(140, 230, 160);

        canvas.fillRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, card_bg);
        canvas.drawRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, border_color);

        // バッジ
        canvas.fillRoundRect(kBadgeX, kCardY + kBadgeYOff, kBadgeW, kBadgeH, kBadgeRadius, border_color);
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(0, 0, 0));
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString("JOY-C", kBadgeCenterX, kCardY + kBadgeCenterYOff);

        // タイトル
        canvas.setFont(kFontTitle);
        canvas.setTextColor(border_color);
        canvas.setTextDatum(lgfx::textdatum_t::middle_left);
        canvas.drawString("JoyC操作 (ESP-NOW)", kTitleX, kCardY + kBadgeCenterYOff);

        // 説明文
        canvas.setFont(kFontDesc);
        canvas.setTextColor(sub_color);
        canvas.setTextDatum(lgfx::textdatum_t::top_left);
        canvas.drawString("プロポ手動操縦 / ラジコン走行モード", kDescX, kCardY + kDescYOff);
    }

    // --- フッター: Wi-Fiステータス / IP表示 / 警告表示 ---
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    // 左下: タップ時の警告メッセージ
    if (now_ms < s_disabled_alert_until_ms) {
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(255, 90, 90)); // 赤色警告
        canvas.setTextDatum(lgfx::textdatum_t::bottom_left);
        canvas.drawString("※Wi-Fiに接続してください", 10, 238);
    }

    // 右下: Wi-Fi接続ステータス & IP表示
    char ip_str[32] = {0};
    if (wifi_get_ip(ip_str, sizeof(ip_str))) {
        char msg[48];
        std::snprintf(msg, sizeof(msg), "IP: %s", ip_str);
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(100, 220, 140)); // エメラルドグリーン
        canvas.setTextDatum(lgfx::textdatum_t::bottom_right);
        canvas.drawString(msg, 314, 238);
    } else {
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(140, 140, 145)); // グレー
        canvas.setTextDatum(lgfx::textdatum_t::bottom_right);
        canvas.drawString("Wi-Fi: 未接続", 314, 238);
    }

    return true;
}

bool handle_tap(int x, int y)
{
    if (!g_active || g_state == nullptr) return false;

    ESP_LOGI(kTag, "Mode select screen tapped at (%d, %d)", x, y);

    if (y < 78) {
        // 上段: 自律運転（距離センサー）
        ESP_LOGI(kTag, "Selected: SonicOnly mode");
        AtomicMotionClient::set_drive_type(AtomicMotionClient::DriveType::SonicOnly, *g_state);
        hide();
        return true;
    } else if (y < 152) {
        // 中段: 自律運転（距離＋カメラ）
        if (!wifi_is_connected()) {
            ESP_LOGW(kTag, "SonicCamera tapped but Wi-Fi not connected. Tap ignored.");
            // 警告表示を2.5秒間トリガー（フッター左下に案内表示）
            s_disabled_alert_until_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000) + 2500;
            return true; // 画面遷移せずイベント消費
        }
        ESP_LOGI(kTag, "Selected: SonicCamera mode");
        AtomicMotionClient::set_drive_type(AtomicMotionClient::DriveType::SonicCamera, *g_state);
        hide();
        return true;
    } else {
        // 下段: JoyC操作（ESPNow）
        ESP_LOGI(kTag, "Selected: JoyCManual mode");
        AtomicMotionClient::set_drive_type(AtomicMotionClient::DriveType::JoyCManual, *g_state);
        hide();
        return true;
    }
}

} // namespace stackchan::app::mode_select
