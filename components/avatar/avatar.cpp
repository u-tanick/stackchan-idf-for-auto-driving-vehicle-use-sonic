// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar/avatar.hpp"

#include <M5GFX.h>

#include "animation.hpp"
#include "balloon.hpp"
#include "effect.hpp"
#include "face.hpp"

namespace stackchan::avatar {

namespace {
constexpr std::int16_t kCanvasWidth = 320;
constexpr std::int16_t kCanvasHeight = 240;
} // namespace

// Avatar is a pure renderer: it owns no display or framebuffer. The caller (the
// render task in main) owns the canvas and pushes it to the panel; tick() only
// composes the frame into the borrowed canvas.
class Avatar::Impl {
public:
    Impl() noexcept = default;

    void tick(std::uint32_t now_ms, M5Canvas& canvas)
    {
        animator_.tick(now_ms, context_);
        context_.now_ms = now_ms;

        canvas.fillScreen(context_.palette.background);
        internal::draw_face(canvas, face_, context_);
        internal::draw_effect(canvas, context_);
        internal::draw_balloon(canvas, context_);
    }

    DrawContext& context() noexcept { return context_; }

    void set_face_tuning(const FaceTuning& tuning)
    {
        face_ = internal::build_face(tuning, kCanvasWidth, kCanvasHeight);
        context_.palette.primary = tuning.face_color;
        context_.palette.background = tuning.bg_color;
    }

private:
    DrawContext context_{};
    internal::Face face_{};
    internal::FaceAnimator animator_{};
};

Avatar::Avatar() : impl_{std::make_unique<Impl>()} {}
Avatar::~Avatar() = default;
Avatar::Avatar(Avatar&&) noexcept = default;
Avatar& Avatar::operator=(Avatar&&) noexcept = default;

void Avatar::set_expression(Expression expression) noexcept
{
    impl_->context().expression = expression;
}

void Avatar::set_mouth_open(float ratio) noexcept
{
    if (ratio < 0.0f) {
        ratio = 0.0f;
    } else if (ratio > 1.0f) {
        ratio = 1.0f;
    }
    impl_->context().mouth_open_ratio = ratio;
}

void Avatar::set_gaze(float horizontal, float vertical) noexcept
{
    impl_->context().gaze_horizontal = horizontal;
    impl_->context().gaze_vertical = vertical;
}

void Avatar::set_palette(const Palette& palette) noexcept
{
    impl_->context().palette = palette;
}

void Avatar::set_face_tuning(const FaceTuning& tuning) noexcept
{
    impl_->set_face_tuning(tuning);
}

void Avatar::set_balloon_text(std::string_view text, std::uint32_t hold_ms)
{
    auto& ctx = impl_->context();
    ctx.balloon_text = std::string{text};
    ctx.balloon_hold_ms = hold_ms;
    ctx.balloon_done = false;
    // Resync marquee phase to "now" so a fresh string always enters from the
    // right edge. context_.now_ms was last updated by tick().
    ctx.balloon_set_ms = ctx.now_ms;
}

void Avatar::clear_balloon() noexcept
{
    auto& ctx = impl_->context();
    ctx.balloon_text.reset();
    ctx.balloon_hold_ms = 0;
    ctx.balloon_done = false;
}

bool Avatar::is_balloon_done() const noexcept
{
    return impl_->context().balloon_done;
}

void Avatar::tick(std::uint32_t now_ms, M5Canvas& canvas)
{
    impl_->tick(now_ms, canvas);
}

} // namespace stackchan::avatar
