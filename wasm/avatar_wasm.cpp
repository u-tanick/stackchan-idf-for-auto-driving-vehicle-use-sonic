// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Emscripten glue for the avatar face renderer. Reuses the firmware's drawing
// (draw_face / draw_effect) and idle animators (FaceAnimator) verbatim,
// rendering into an in-memory RGB565 canvas (see wasm/shim/M5GFX.h). The
// browser reads the framebuffer each frame and blits it to a <canvas>.

#include <algorithm>
#include <cstdint>

#include <emscripten/emscripten.h>

#include "animation.hpp"
#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/face_tuning.hpp"
#include "avatar/palette.hpp"
#include "effect.hpp"
#include "avatar/canvas.hpp"
#include "face.hpp"

using namespace stackchan::avatar;

namespace {

// Browser preview always has a full framebuffer, so this is the "buffered"
// strategy: forward the Canvas primitives straight to the shim M5Canvas and
// treat the grouping / frame hooks as no-ops (begin_frame just clears).
class WasmCanvas final : public Canvas {
public:
    explicit WasmCanvas(M5Canvas& c) noexcept : c_{c} {}
    std::int32_t width() const override { return c_.width(); }
    std::int32_t height() const override { return c_.height(); }
    void fillScreen(std::uint16_t color) override { c_.fillScreen(color); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        c_.fillRect(x, y, w, h, color);
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        c_.fillCircle(x, y, r, color);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        c_.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }
    void begin_group(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override {}
    void end_group() override {}
    void begin_frame(std::uint16_t bg) override { c_.fillScreen(bg); }
    void end_frame() override {}
    void request_full_repaint() override {}

private:
    M5Canvas& c_;
};

// Mirror of the firmware DirectCanvas (canvas_m5gfx.hpp) for browser debugging:
// the framebuffer is treated as a *persistent panel* (only cleared on a full
// repaint), narrow multi-primitive groups composite into a grow-only scratch
// and blit clipped to the group rect, and wide groups (the balloon) draw
// straight to the panel. Lets us reproduce the direct-strategy rendering (and
// its bugs) headlessly in the browser.
class WasmDirectCanvas final : public Canvas {
public:
    explicit WasmDirectCanvas(M5Canvas& panel) noexcept : panel_{panel} {}

    std::int32_t width() const override { return panel_.width(); }
    std::int32_t height() const override { return panel_.height(); }

    void fillScreen(std::uint16_t color) override { tgt().fillScreen(color); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        tgt().fillRect(x - ox_, y - oy_, w, h, color);
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        tgt().fillCircle(x - ox_, y - oy_, r, color);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        tgt().fillTriangle(x0 - ox_, y0 - oy_, x1 - ox_, y1 - oy_, x2 - ox_, y2 - oy_, color);
    }
    void begin_group(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        std::int32_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
        std::int32_t x1 = x + w, y1 = y + h;
        if (x1 > panel_.width()) x1 = panel_.width();
        if (y1 > panel_.height()) y1 = panel_.height();
        gx_ = x0; gy_ = y0; gw_ = x1 - x0; gh_ = y1 - y0;
        if (gw_ <= 0 || gh_ <= 0 || gw_ > kMaxScratchWidth || !ensure_scratch(gw_, gh_)) {
            in_group_ = false; ox_ = oy_ = 0; return;
        }
        ox_ = gx_; oy_ = gy_; in_group_ = true;
        scratch_.fillRect(0, 0, gw_, gh_, bg_);
    }
    void end_group() override
    {
        if (!in_group_) return;
        in_group_ = false; ox_ = oy_ = 0;
        // Blit scratch[0..gw,0..gh] → panel[gx..,gy..] (clipped to the panel).
        const std::uint16_t* src = scratch_.getBuffer();
        std::uint16_t* dst = panel_.getBuffer();
        const std::int32_t sw = scratch_.width();
        const std::int32_t pw = panel_.width(), ph = panel_.height();
        if (src == nullptr || dst == nullptr) return;
        for (std::int32_t r = 0; r < gh_; ++r) {
            const std::int32_t py = gy_ + r;
            if (py < 0 || py >= ph) continue;
            for (std::int32_t c = 0; c < gw_; ++c) {
                const std::int32_t px = gx_ + c;
                if (px < 0 || px >= pw) continue;
                dst[py * pw + px] = src[r * sw + c];
            }
        }
    }
    void begin_frame(std::uint16_t bg) override
    {
        bg_ = bg;
        if (full_repaint_pending_) {
            panel_.fillScreen(bg);
            full_repaint_pending_ = false;
        }
    }
    void end_frame() override {}
    void request_full_repaint() override { full_repaint_pending_ = true; }

private:
    M5Canvas& tgt() { return in_group_ ? scratch_ : panel_; }
    bool ensure_scratch(std::int32_t w, std::int32_t h)
    {
        if (w <= scratch_w_ && h <= scratch_h_) return true;
        const std::int32_t nw = w > scratch_w_ ? w : scratch_w_;
        const std::int32_t nh = h > scratch_h_ ? h : scratch_h_;
        scratch_.deleteSprite();
        if (scratch_.createSprite(nw, nh) != nullptr) {
            scratch_w_ = nw; scratch_h_ = nh; return true;
        }
        scratch_w_ = scratch_h_ = 0; return false;
    }

    static constexpr std::int32_t kMaxScratchWidth = 160;
    M5Canvas& panel_;
    M5Canvas scratch_;
    std::int32_t scratch_w_ = 0, scratch_h_ = 0;
    std::uint16_t bg_ = 0;
    std::int32_t gx_ = 0, gy_ = 0, gw_ = 0, gh_ = 0, ox_ = 0, oy_ = 0;
    bool in_group_ = false;
    bool full_repaint_pending_ = true;
};

std::int32_t g_w = 320;
std::int32_t g_h = 240;

M5Canvas g_canvas;
DrawContext g_ctx;
internal::Face g_face;
internal::FaceAnimator g_anim;

WasmDirectCanvas g_direct{g_canvas}; // mirrors firmware DirectCanvas, for debugging
bool g_direct_mode = false;
int g_last_expr = -1; // tracks expression changes to trigger a full repaint

bool g_manual_gaze = false;
float g_gaze_h = 0.0f;
float g_gaze_v = 0.0f;

std::uint32_t clamp_u32(int v) { return v < 0 ? 0u : static_cast<std::uint32_t>(v); }

// Adjustable face layout. Defaults mirror the firmware's internal::Face so the
// initial render is identical. Colours are driven separately via
// avatar_set_colors (g_ctx.palette), so g_tune's colour fields are unused here.
FaceTuning g_tune;

void rebuild_face()
{
    g_face = internal::build_face(g_tune, static_cast<std::int16_t>(g_w),
                                  static_cast<std::int16_t>(g_h));
    g_direct.request_full_repaint(); // layout changed — clear the persistent panel
}
} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE int avatar_init()
{
    if (g_canvas.createSprite(g_w, g_h) == nullptr) {
        return 0;
    }
    g_ctx = DrawContext{};
    rebuild_face();
    return 1;
}

// Resize the render canvas (clamped 64..1280 x 32..720). Reallocates the
// framebuffer (callers must re-read avatar_framebuffer()) and rescales the
// face layout to fill the new size. Returns 1 on success.
EMSCRIPTEN_KEEPALIVE int avatar_set_size(int w, int h)
{
    if (w < 64) w = 64;
    if (w > 1280) w = 1280;
    if (h < 32) h = 32;
    if (h > 720) h = 720;
    if (w == g_w && h == g_h) {
        return 1;
    }
    g_w = w;
    g_h = h;
    g_canvas.deleteSprite();
    if (g_canvas.createSprite(g_w, g_h) == nullptr) {
        return 0;
    }
    rebuild_face();
    return 1;
}

EMSCRIPTEN_KEEPALIVE int avatar_width() { return g_w; }
EMSCRIPTEN_KEEPALIVE int avatar_height() { return g_h; }
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

// ---- face layout tuning ------------------------------------------------

EMSCRIPTEN_KEEPALIVE void avatar_set_eyebrows_visible(int on)
{
    g_tune.eyebrows_visible = on != 0;
    rebuild_face();
}

// radius = eye size; off_x spreads the eyes apart (symmetric), off_y moves
// both vertically.
EMSCRIPTEN_KEEPALIVE void avatar_set_eye_params(float radius, float off_x, float off_y)
{
    g_tune.eye_radius = radius < 1.0f ? 1.0f : radius;
    g_tune.eye_off_x = off_x;
    g_tune.eye_off_y = off_y;
    rebuild_face();
}

EMSCRIPTEN_KEEPALIVE void avatar_set_eyebrow_params(float off_x, float off_y)
{
    g_tune.brow_off_x = off_x;
    g_tune.brow_off_y = off_y;
    rebuild_face();
}

// off_x/off_y move the mouth; min_w/max_w set the (closed/open) width range;
// min_h/max_h set the open-amount (closed/fully-open height) range.
EMSCRIPTEN_KEEPALIVE void avatar_set_mouth_params(float off_x, float off_y,
                                                  int min_w, int max_w, int min_h, int max_h)
{
    g_tune.mouth_off_x = off_x;
    g_tune.mouth_off_y = off_y;
    g_tune.mouth_min_w = min_w;
    g_tune.mouth_max_w = max_w;
    g_tune.mouth_min_h = min_h;
    g_tune.mouth_max_h = max_h;
    rebuild_face();
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
    g_direct.request_full_repaint(); // background colour changed
}

// Toggle the direct rendering strategy (mirrors the firmware PSRAM-less path).
// 0 = buffered (full-frame clear + redraw), 1 = direct (persistent panel +
// per-element group composite). Forces a full repaint on the next frame.
EMSCRIPTEN_KEEPALIVE void avatar_set_direct(int on)
{
    g_direct_mode = on != 0;
    g_direct.request_full_repaint();
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

    const int expr = static_cast<int>(g_ctx.expression);
    if (g_direct_mode) {
        if (expr != g_last_expr) {
            g_direct.request_full_repaint(); // effect appears/disappears, masks change
            g_last_expr = expr;
        }
        g_direct.begin_frame(g_ctx.palette.background);
        internal::draw_face(g_direct, g_face, g_ctx);
        internal::draw_effect(g_direct, g_ctx);
        g_direct.end_frame();
    } else {
        g_last_expr = expr;
        WasmCanvas canvas{g_canvas};
        canvas.begin_frame(g_ctx.palette.background);
        internal::draw_face(canvas, g_face, g_ctx);
        internal::draw_effect(canvas, g_ctx);
    }
}

} // extern "C"
