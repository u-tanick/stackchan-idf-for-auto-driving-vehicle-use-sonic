// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "render_task.hpp"

#include <cstdio>
#include <string>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "avatar/avatar.hpp"
#include "avatar/canvas.hpp"
#include "avatar/canvas_m5gfx.hpp"
#include "device_ui.hpp"
#include "face_config.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "render";
constexpr TickType_t kPeriodTicks = pdMS_TO_TICKS(33);
constexpr std::int32_t kCanvasWidth = 320;
constexpr std::int32_t kCanvasHeight = 240;

using avatar::RichCanvas;

// Battery gauge overlay, composited into the frame just before present (same
// frame as the face — drawing after present flickers). `pct` is 0..100; values
// < 0 are filtered out by the caller. Wrapped in a group so the direct strategy
// composites it off-screen.
void draw_battery_gauge(RichCanvas& canvas, int pct)
{
    constexpr int x = 6, y = 6, w = 34, h = 16; // battery body
    constexpr int nub_w = 3, nub_h = 6;          // positive terminal nub
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const std::uint16_t white = canvas.color565(235, 235, 235);
    const std::uint16_t black = canvas.color565(0, 0, 0);
    const std::uint16_t fill = pct >= 50 ? canvas.color565(80, 220, 120)
                              : (pct >= 20 ? canvas.color565(235, 200, 90)
                                           : canvas.color565(230, 110, 110));

    canvas.begin_group(x - 1, y - 1, w + nub_w + 44, h + 2);
    // Backing panel behind the icon + text so it stays legible over the face.
    canvas.fillRect(x - 1, y - 1, w + nub_w + 44, h + 2, black);
    // Body outline + terminal.
    canvas.drawRoundRect(x, y, w, h, 2, white);
    canvas.fillRect(x + w, y + (h - nub_h) / 2, nub_w, nub_h, white);
    // Charge fill proportional to percent.
    const int inner_w = w - 4;
    const int filled = (inner_w * pct + 50) / 100;
    if (filled > 0) {
        canvas.fillRect(x + 2, y + 2, filled, h - 4, fill);
    }
    // Percent text to the right of the icon.
    char label[8];
    std::snprintf(label, sizeof(label), "%d%%", pct);
    canvas.setTextDatum(lgfx::textdatum_t::middle_left);
    canvas.setTextColor(white, black);
    canvas.setTextSize(1);
    canvas.drawString(label, x + w + nub_w + 4, y + h / 2);
    canvas.end_group();
}

void render_task_entry(void* arg)
{
    auto& args = *static_cast<RenderTaskArgs*>(arg);
    M5GFX& display = *args.display;
    SharedState* state = args.state;

    // The drawing strategy (= the system framebuffer) is owned here (main), not
    // by the avatar / on-device UI — they render through the abstract Canvas.
    // Buffered strategy: one full-screen PSRAM sprite, pushed once per frame.
    // (DirectCanvas for PSRAM-less devices is selected here in a later step.)
    avatar::BufferedCanvas buffered{display};
    if (!buffered.begin(kCanvasWidth, kCanvasHeight)) {
        ESP_LOGE(kTag, "framebuffer createSprite(%d, %d) failed (need PSRAM)",
                 static_cast<int>(kCanvasWidth), static_cast<int>(kCanvasHeight));
        vTaskDelete(nullptr);
        return;
    }
    RichCanvas& canvas = buffered;

    avatar::Avatar avatar;

    int last_expression = -1;
    std::uint32_t last_balloon_version = 0;
    std::uint32_t last_face_config_version = 0;
    std::string balloon_scratch;
    bool balloon_pending = false;
    bool ui_was_active = false;

    for (;;) {
        const std::uint32_t now_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);

        // On-device touchscreen UI takes over the display while shown. It draws
        // into the shared canvas (lazily — only when something changed); push
        // only when it actually repainted.
        if (ui::active()) {
            // device_ui clears + draws itself (lazily); present only when it
            // repainted this frame.
            if (ui::draw(canvas)) {
                canvas.end_frame();
            }
            ui_was_active = true;
            vTaskDelay(kPeriodTicks);
            continue;
        }
        if (ui_was_active) {
            // Returning to the avatar — force a full repaint so the direct
            // strategy clears the whole panel (UI content) before redrawing.
            ui_was_active = false;
            avatar.request_full_repaint();
            last_expression = -1; // force a fresh expression apply
        }

        // Live face-tuning updates (BLE settings UI / boot-time NVS restore).
        // Parsing the JSON here keeps it off the BLE host task's small stack.
        const std::uint32_t face_config_version = args.state->face_config_version();
        if (face_config_version != last_face_config_version) {
            avatar.set_face_tuning(parse_face_tuning(args.state->snapshot_face_config()));
            last_face_config_version = face_config_version;
        }

        const int expr = args.state->expression.load(std::memory_order_relaxed);
        if (expr != last_expression) {
            avatar.set_expression(static_cast<avatar::Expression>(expr));
            last_expression = expr;
        }
        avatar.set_mouth_open(args.state->mouth_open.load(std::memory_order_relaxed));

        const std::uint32_t balloon_version = args.state->balloon_version();
        if (balloon_version != last_balloon_version) {
            if (args.state->balloon_visible()) {
                std::uint32_t hold_ms = 0;
                args.state->snapshot_balloon(balloon_scratch, hold_ms);
                avatar.set_balloon_text(balloon_scratch, hold_ms);
                balloon_pending = true;
            } else {
                avatar.clear_balloon();
                balloon_pending = false;
            }
            last_balloon_version = balloon_version;
        }

        // avatar.tick() opens the frame (begin_frame) and draws the face;
        // overlays compose into the same frame; end_frame() presents.
        avatar.tick(now_ms, canvas);

        // Battery gauge overlay. Live from SharedState.
        if (state->battery_gauge_enabled.load(std::memory_order_relaxed)) {
            const int pct = state->battery_pct.load(std::memory_order_relaxed);
            if (pct >= 0) {
                draw_battery_gauge(canvas, pct);
            }
        }

        canvas.end_frame();

        if (balloon_pending && avatar.is_balloon_done()) {
            balloon_pending = false;
            args.state->notify_balloon_complete();
        }

        // Use vTaskDelay (not vTaskDelayUntil) so the IDLE task on this core
        // always gets at least one tick, even if a frame ran long.
        vTaskDelay(kPeriodTicks);
    }
}

} // namespace

void start_render_task(RenderTaskArgs& args)
{
    xTaskCreatePinnedToCore(render_task_entry, "render", 8192, &args, 5, nullptr, 1);
}

} // namespace stackchan::app
