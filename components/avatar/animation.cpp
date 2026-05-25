// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "animation.hpp"

#include <cmath>
#include <numbers>

namespace stackchan::avatar::internal {

void FaceAnimator::breath_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    breath_phase_ = (breath_phase_ + 1) % 100;
    const float f = std::sin(static_cast<float>(breath_phase_) * 2.0f * std::numbers::pi_v<float> / 100.0f);
    ctx.breath = f;
    breath_next_ms_ = now_ms + 33;
}

void FaceAnimator::saccade_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    XorShift32 rng{ctx.rng_state};
    const float amp = params.gaze_amplitude;
    ctx.gaze_horizontal = rng.next_range(-amp, amp);
    ctx.gaze_vertical = rng.next_range(-amp, amp);
    const std::uint32_t lo = params.saccade_min_ms;
    const std::uint32_t hi = params.saccade_max_ms >= lo ? params.saccade_max_ms : lo;
    const std::uint32_t delay = rng.next_inclusive(lo, hi);
    ctx.rng_state = rng.next();
    saccade_next_ms_ = now_ms + delay;
}

void FaceAnimator::blink_tick(std::uint32_t now_ms, DrawContext& ctx)
{
    XorShift32 rng{ctx.rng_state};
    eyes_open_ = !eyes_open_;
    std::uint32_t delay;
    if (eyes_open_) {
        ctx.eye_open_ratio = 1.0f;
        const std::uint32_t lo = params.blink_open_min_ms;
        const std::uint32_t hi = params.blink_open_max_ms >= lo ? params.blink_open_max_ms : lo;
        delay = rng.next_inclusive(lo, hi);
    } else {
        ctx.eye_open_ratio = 0.0f;
        const std::uint32_t lo = params.blink_closed_min_ms;
        const std::uint32_t hi = params.blink_closed_max_ms >= lo ? params.blink_closed_max_ms : lo;
        delay = rng.next_inclusive(lo, hi);
    }
    ctx.rng_state = rng.next();
    blink_next_ms_ = now_ms + delay;
}

void FaceAnimator::tick(std::uint32_t now_ms, DrawContext& ctx)
{
    if (params.breath_enabled) {
        if (now_ms >= breath_next_ms_) {
            breath_tick(now_ms, ctx);
        }
    } else {
        ctx.breath = 0.0f;
    }
    if (params.saccade_enabled) {
        if (now_ms >= saccade_next_ms_) {
            saccade_tick(now_ms, ctx);
        }
    }
    if (params.blink_enabled) {
        if (now_ms >= blink_next_ms_) {
            blink_tick(now_ms, ctx);
        }
    } else {
        ctx.eye_open_ratio = 1.0f;
        eyes_open_ = true;
    }
}

} // namespace stackchan::avatar::internal
