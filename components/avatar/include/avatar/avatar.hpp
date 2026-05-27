// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <M5GFX.h>

#include "avatar/draw_context.hpp"
#include "avatar/expression.hpp"
#include "avatar/face_tuning.hpp"
#include "avatar/palette.hpp"

namespace stackchan::avatar {

class Avatar {
public:
    Avatar();
    ~Avatar();

    Avatar(const Avatar&) = delete;
    Avatar& operator=(const Avatar&) = delete;
    Avatar(Avatar&&) noexcept;
    Avatar& operator=(Avatar&&) noexcept;

    void set_expression(Expression expression) noexcept;
    void set_mouth_open(float ratio) noexcept;
    void set_gaze(float horizontal, float vertical) noexcept;
    void set_palette(const Palette& palette) noexcept;
    // Rebuild the face layout from user tuning (eye/eyebrow/mouth geometry) and
    // apply its face/background colours. Takes effect on the next tick(); safe
    // to call live (e.g. from the render task on a config change).
    void set_face_tuning(const FaceTuning& tuning) noexcept;
    // Show `text` in the balloon. `hold_ms` overrides the default display
    // time (0 = use balloon defaults: short text holds for a few seconds,
    // long text plays one full marquee pass).
    void set_balloon_text(std::string_view text, std::uint32_t hold_ms = 0);
    void clear_balloon() noexcept;
    // True once the current balloon has been fully displayed (hold elapsed
    // or one marquee pass completed). Stays true until the next
    // set_balloon_text / clear_balloon.
    bool is_balloon_done() const noexcept;

    // Drives the animators with the current time (ms) and renders one frame into
    // `canvas` (fills the background then draws face / effect / balloon). The
    // caller owns the canvas and is responsible for pushing it to the panel —
    // Avatar never touches the display. The canvas is expected to be 320x240.
    void tick(std::uint32_t now_ms, M5Canvas& canvas);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace stackchan::avatar
