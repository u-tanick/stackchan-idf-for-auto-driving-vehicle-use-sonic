// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "mode_select_screen.hpp"
#include "atomic_motion_client.hpp"
#include <esp_log.h>

namespace stackchan::app::mode_select {

namespace {

constexpr const char* kTag = "mode_select";

SharedState* g_state = nullptr;
bool g_active = false;

const auto* const kFontTitle = &fonts::lgfxJapanGothic_16;
const auto* const kFontDesc  = &fonts::lgfxJapanGothic_12;

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

    // 縦3分割カードの共通設定
    constexpr int32_t kCardX = 6;
    constexpr int32_t kCardW = 308;
    constexpr int32_t kCardH = 70;
    constexpr int32_t kRadius = 8;

    // バッジ共通設定（3つすべて同一サイズ、VLM AIが余裕をもって収まる幅56px）
    constexpr int32_t kBadgeX = kCardX + 10;
    constexpr int32_t kBadgeYOff = 12;
    constexpr int32_t kBadgeW = 56;
    constexpr int32_t kBadgeH = 20;
    constexpr int32_t kBadgeRadius = 4;
    constexpr int32_t kBadgeCenterX = kBadgeX + kBadgeW / 2;
    constexpr int32_t kBadgeCenterYOff = kBadgeYOff + kBadgeH / 2;

    // タイトルおよび説明文の共通配置
    constexpr int32_t kTitleX = kBadgeX + kBadgeW + 8; // kCardX + 74
    constexpr int32_t kDescX = kCardX + 12;
    constexpr int32_t kDescYOff = 44;

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
        constexpr int32_t kCardY = 85;
        const uint16_t card_bg = canvas.color565(48, 28, 12);
        const uint16_t border_color = canvas.color565(255, 150, 0); // Orange
        const uint16_t sub_color  = canvas.color565(240, 190, 140);

        canvas.fillRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, card_bg);
        canvas.drawRoundRect(kCardX, kCardY, kCardW, kCardH, kRadius, border_color);

        // バッジ
        canvas.fillRoundRect(kBadgeX, kCardY + kBadgeYOff, kBadgeW, kBadgeH, kBadgeRadius, border_color);
        canvas.setFont(kFontDesc);
        canvas.setTextColor(canvas.color565(0, 0, 0));
        canvas.setTextDatum(lgfx::textdatum_t::middle_center);
        canvas.drawString("VLM AI", kBadgeCenterX, kCardY + kBadgeCenterYOff);

        // タイトル
        canvas.setFont(kFontTitle);
        canvas.setTextColor(border_color);
        canvas.setTextDatum(lgfx::textdatum_t::middle_left);
        canvas.drawString("自律運転 (距離＋カメラ)", kTitleX, kCardY + kBadgeCenterYOff);

        // 説明文
        canvas.setFont(kFontDesc);
        canvas.setTextColor(sub_color);
        canvas.setTextDatum(lgfx::textdatum_t::top_left);
        canvas.drawString("AI画像認識で4方向探索・最適ルート決定", kDescX, kCardY + kDescYOff);
    }

    // --- 下段: JoyC操作（ESPNow） ---
    {
        constexpr int32_t kCardY = 164;
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

    return true;
}

bool handle_tap(int x, int y)
{
    if (!g_active || g_state == nullptr) return false;

    ESP_LOGI(kTag, "Mode select screen tapped at (%d, %d)", x, y);

    if (y < 80) {
        // 上段: 自律運転（距離センサー）
        ESP_LOGI(kTag, "Selected: SonicOnly mode");
        AtomicMotionClient::set_drive_type(AtomicMotionClient::DriveType::SonicOnly, *g_state);
        hide();
        return true;
    } else if (y < 160) {
        // 中段: 自律運転（距離＋カメラ）
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
