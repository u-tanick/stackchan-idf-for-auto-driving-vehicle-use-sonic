// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <M5Unified.h>

#include "avatar/canvas.hpp"

// Firmware-only concrete Canvas strategies. NOT compiled for the WASM preview
// (which supplies its own Canvas adapter over the framebuffer shim).
namespace stackchan::avatar {

// Buffered strategy: composes the whole frame into one full-screen RGB565
// sprite (PSRAM) and pushes it to the panel once per frame. Reproduces the
// original avatar rendering exactly. Drawing primitives forward straight to the
// owned sprite; the grouping / full-repaint hooks are no-ops because everything
// already composites into the single buffer.
class BufferedCanvas final : public RichCanvas {
public:
    explicit BufferedCanvas(M5GFX& panel) noexcept : panel_{panel} {}

    // Allocate the full-screen sprite. Standalone (no display parent) + explicit
    // target on present, to dodge the CoreS3 GPIO35 MISO/DC read hang. Returns
    // false if PSRAM couldn't satisfy the allocation.
    bool begin(std::int32_t w, std::int32_t h)
    {
        sprite_.setColorDepth(16);
        sprite_.setPsram(true);
        return sprite_.createSprite(w, h) != nullptr;
    }

    std::int32_t width() const override { return sprite_.width(); }
    std::int32_t height() const override { return sprite_.height(); }

    void fillScreen(std::uint16_t color) override { sprite_.fillScreen(color); }
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        sprite_.fillRect(x, y, w, h, color);
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        sprite_.fillCircle(x, y, r, color);
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        sprite_.fillTriangle(x0, y0, x1, y1, x2, y2, color);
    }

    void begin_group(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override {}
    void end_group() override {}
    void begin_frame(std::uint16_t bg) override { sprite_.fillScreen(bg); }
    void end_frame() override { sprite_.pushSprite(&panel_, 0, 0); }
    void request_full_repaint() override {}

    std::uint16_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) override
    {
        return sprite_.color565(r, g, b);
    }
    void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        sprite_.fillRoundRect(x, y, w, h, r, color);
    }
    void drawRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint16_t color) override
    {
        sprite_.drawRoundRect(x, y, w, h, r, color);
    }
    void setTextColor(std::uint16_t fg) override { sprite_.setTextColor(fg); }
    void setTextColor(std::uint16_t fg, std::uint16_t bg) override { sprite_.setTextColor(fg, bg); }
    void setFont(const lgfx::IFont* font) override { sprite_.setFont(font); }
    void setTextSize(float size) override { sprite_.setTextSize(size); }
    void setTextDatum(lgfx::textdatum_t datum) override { sprite_.setTextDatum(datum); }
    void drawString(const char* str, std::int32_t x, std::int32_t y) override
    {
        sprite_.drawString(str, x, y);
    }
    std::int32_t textWidth(const char* str) override { return sprite_.textWidth(str); }
    void setClipRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        sprite_.setClipRect(x, y, w, h);
    }
    void clearClipRect() override { sprite_.clearClipRect(); }

private:
    M5GFX& panel_;
    M5Canvas sprite_;
};

} // namespace stackchan::avatar
