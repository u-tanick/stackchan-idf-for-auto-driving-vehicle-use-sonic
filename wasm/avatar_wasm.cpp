// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Emscripten glue for the avatar face renderer. Reuses the firmware's drawing
// (draw_face / draw_effect) and idle animators (FaceAnimator) verbatim,
// rendering into an in-memory RGB565 canvas (see wasm/shim/M5GFX.h). The
// browser reads the framebuffer each frame and blits it to a <canvas>.

#include <cstdint>

#include <emscripten/emscripten.h>

#include "animation.hpp"
#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/palette.hpp"
#include "effect.hpp"
#include "face.hpp"

using namespace stackchan::avatar;

namespace {
constexpr std::int32_t kWidth = 320;
constexpr std::int32_t kHeight = 240;

M5Canvas g_canvas;
DrawContext g_ctx;
internal::Face g_face;
internal::FaceAnimator g_anim;

bool g_manual_gaze = false;
float g_gaze_h = 0.0f;
float g_gaze_v = 0.0f;

std::uint32_t clamp_u32(int v) { return v < 0 ? 0u : static_cast<std::uint32_t>(v); }
} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int avatar_init()
{
    if (g_canvas.createSprite(kWidth, kHeight) == nullptr) {
        return 0;
    }
    g_ctx = DrawContext{};
    return 1;
}

EMSCRIPTEN_KEEPALIVE int avatar_width() { return kWidth; }
EMSCRIPTEN_KEEPALIVE int avatar_height() { return kHeight; }
EMSCRIPTEN_KEEPALIVE std::uint16_t* avatar_framebuffer() { return g_canvas.getBuffer(); }

// expression: 0=Neutral 1=Happy 2=Sad 3=Angry 4=Doubt 5=Sleepy
EMSCRIPTEN_KEEPALIVE void avatar_set_expression(int e)
{
    if (e < 0) e = 0;
    if (e > 5) e = 5;
    g_ctx.expression = static_cast<Expression>(e);
}

EMSCRIPTEN_KEEPALIVE void avatar_set_mouth(float ratio)
{
    g_ctx.mouth_open_ratio = ratio < 0.0f ? 0.0f : (ratio > 1.0f ? 1.0f : ratio);
}

// Override the gaze (disables the effect of saccade for this frame). on=0
// returns control to the saccade animator.
EMSCRIPTEN_KEEPALIVE void avatar_set_manual_gaze(int on, float h, float v)
{
    g_manual_gaze = on != 0;
    g_gaze_h = h;
    g_gaze_v = v;
}

EMSCRIPTEN_KEEPALIVE void avatar_set_saccade(int enabled, int min_ms, int max_ms, float amplitude)
{
    g_anim.params.saccade_enabled = enabled != 0;
    g_anim.params.saccade_min_ms = clamp_u32(min_ms);
    g_anim.params.saccade_max_ms = clamp_u32(max_ms);
    g_anim.params.gaze_amplitude = amplitude < 0.0f ? 0.0f : amplitude;
}

EMSCRIPTEN_KEEPALIVE void avatar_set_blink(int enabled, int open_min, int open_max,
                                           int closed_min, int closed_max)
{
    g_anim.params.blink_enabled = enabled != 0;
    g_anim.params.blink_open_min_ms = clamp_u32(open_min);
    g_anim.params.blink_open_max_ms = clamp_u32(open_max);
    g_anim.params.blink_closed_min_ms = clamp_u32(closed_min);
    g_anim.params.blink_closed_max_ms = clamp_u32(closed_max);
}

EMSCRIPTEN_KEEPALIVE void avatar_set_breath(int enabled)
{
    g_anim.params.breath_enabled = enabled != 0;
}

// Set the RGB565 face / background colours (0xRRGGBB inputs converted here).
EMSCRIPTEN_KEEPALIVE void avatar_set_colors(int face_rgb, int bg_rgb)
{
    auto to565 = [](int rgb) -> std::uint16_t {
        const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
        return static_cast<std::uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    };
    g_ctx.palette.primary = to565(face_rgb);
    g_ctx.palette.background = to565(bg_rgb);
}

// Render one frame at the given wall-clock time (ms). Mirrors Avatar::tick().
EMSCRIPTEN_KEEPALIVE void avatar_tick(double now_ms)
{
    const std::uint32_t t = static_cast<std::uint32_t>(now_ms);
    g_anim.tick(t, g_ctx);
    if (g_manual_gaze) {
        g_ctx.gaze_horizontal = g_gaze_h;
        g_ctx.gaze_vertical = g_gaze_v;
    }
    g_ctx.now_ms = t;

    g_canvas.fillScreen(g_ctx.palette.background);
    internal::draw_face(g_canvas, g_face, g_ctx);
    internal::draw_effect(g_canvas, g_ctx);
}

} // extern "C"
