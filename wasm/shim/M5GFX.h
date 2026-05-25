// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Minimal M5GFX / M5Canvas shim for building the avatar drawing code with
// Emscripten. It implements just the primitives the avatar face uses
// (fillScreen / fillRect / fillCircle / fillTriangle / fillRoundRect) against
// an in-memory RGB565 framebuffer, plus no-op stubs for the text / sprite-push
// API so the headers compile. NOT a general M5GFX replacement.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- lgfx / fonts compatibility shims (only referenced by balloon.cpp, which
//      the WASM build does not compile, but headers may name these types). ----
namespace lgfx {
enum textdatum_t {
    top_left = 0,
    middle_left,
    middle_center,
    middle_right,
};
struct IFont {};
} // namespace lgfx

namespace fonts {
inline lgfx::IFont lgfxJapanGothic_24;
inline lgfx::IFont lgfxJapanGothic_16;
} // namespace fonts

class M5GFX; // forward decl

// RGB565 framebuffer-backed canvas/sprite.
class M5Canvas {
public:
    M5Canvas() = default;
    explicit M5Canvas(M5GFX* /*parent*/) {}
    ~M5Canvas() = default;

    void setColorDepth(int /*bpp*/) noexcept {}
    void setPsram(bool /*use*/) noexcept {}

    void* createSprite(std::int32_t w, std::int32_t h)
    {
        if (w <= 0 || h <= 0) {
            return nullptr;
        }
        w_ = w;
        h_ = h;
        buf_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
        return buf_.data();
    }
    void deleteSprite() noexcept { buf_.clear(); w_ = h_ = 0; }

    std::int32_t width() const noexcept { return w_; }
    std::int32_t height() const noexcept { return h_; }
    std::uint16_t* getBuffer() noexcept { return buf_.data(); }

    void fillScreen(std::uint32_t color) noexcept
    {
        const std::uint16_t c = static_cast<std::uint16_t>(color);
        std::fill(buf_.begin(), buf_.end(), c);
    }

    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint32_t color) noexcept
    {
        const std::uint16_t c = static_cast<std::uint16_t>(color);
        const std::int32_t x0 = std::max<std::int32_t>(0, x);
        const std::int32_t y0 = std::max<std::int32_t>(0, y);
        const std::int32_t x1 = std::min<std::int32_t>(w_, x + w);
        const std::int32_t y1 = std::min<std::int32_t>(h_, y + h);
        for (std::int32_t yy = y0; yy < y1; ++yy) {
            std::uint16_t* row = buf_.data() + static_cast<std::size_t>(yy) * w_;
            for (std::int32_t xx = x0; xx < x1; ++xx) {
                row[xx] = c;
            }
        }
    }

    void fillCircle(std::int32_t cx, std::int32_t cy, std::int32_t r, std::uint32_t color) noexcept
    {
        if (r < 0) return;
        const std::int32_t r2 = r * r;
        for (std::int32_t dy = -r; dy <= r; ++dy) {
            const std::int32_t yy = cy + dy;
            if (yy < 0 || yy >= h_) continue;
            const std::int32_t dx = static_cast<std::int32_t>(std::sqrt(
                static_cast<double>(r2 - dy * dy) + 0.5));
            fill_span(cx - dx, cx + dx, yy, static_cast<std::uint16_t>(color));
        }
    }

    void fillRoundRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                       std::int32_t r, std::uint32_t color) noexcept
    {
        r = std::min({r, w / 2, h / 2});
        if (r <= 0) {
            fillRect(x, y, w, h, color);
            return;
        }
        fillRect(x + r, y, w - 2 * r, h, color);       // centre column
        fillRect(x, y + r, r, h - 2 * r, color);       // left edge
        fillRect(x + w - r, y + r, r, h - 2 * r, color); // right edge
        const std::uint16_t c = static_cast<std::uint16_t>(color);
        fill_corner(x + r, y + r, r, -1, -1, c);
        fill_corner(x + w - r - 1, y + r, r, 1, -1, c);
        fill_corner(x + r, y + h - r - 1, r, -1, 1, c);
        fill_corner(x + w - r - 1, y + h - r - 1, r, 1, 1, c);
    }

    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint32_t color) noexcept
    {
        // Sort vertices by y ascending.
        if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
        if (y0 > y2) { std::swap(y0, y2); std::swap(x0, x2); }
        if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
        const std::uint16_t c = static_cast<std::uint16_t>(color);
        if (y2 == y0) { // degenerate: single scanline
            const std::int32_t a = std::min({x0, x1, x2});
            const std::int32_t b = std::max({x0, x1, x2});
            fill_span(a, b, y0, c);
            return;
        }
        for (std::int32_t y = y0; y <= y2; ++y) {
            // long edge x (v0->v2), spans the full height
            const std::int32_t xa = interp(y, y0, y2, x0, x2);
            // short edge: v0->v1 for the upper sub-triangle, v1->v2 for the
            // lower one. `lower` is also true when the top is flat (y0==y1),
            // where the whole triangle is the v1->v2 / v0->v2 pair.
            const bool lower = (y > y1) || (y0 == y1);
            const std::int32_t xb = lower ? interp(y, y1, y2, x1, x2)
                                          : interp(y, y0, y1, x0, x1);
            fill_span(std::min(xa, xb), std::max(xa, xb), y, c);
        }
    }

    // --- text / sprite API: stubs (balloon is not built for WASM) ---
    void setTextColor(std::uint32_t /*fg*/, std::uint32_t /*bg*/) noexcept {}
    void setTextColor(std::uint32_t /*fg*/) noexcept {}
    void setFont(const void* /*font*/) noexcept {}
    void setTextSize(float /*s*/) noexcept {}
    void setTextDatum(int /*datum*/) noexcept {}
    std::int32_t textWidth(const char* /*s*/) noexcept { return 0; }
    void drawString(const char* /*s*/, std::int32_t /*x*/, std::int32_t /*y*/) noexcept {}
    void setClipRect(std::int32_t /*x*/, std::int32_t /*y*/, std::int32_t /*w*/,
                     std::int32_t /*h*/) noexcept {}
    void clearClipRect() noexcept {}
    void pushSprite(M5GFX* /*dst*/, std::int32_t /*x*/, std::int32_t /*y*/) noexcept {}

    static std::uint32_t color565(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        return static_cast<std::uint32_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

private:
    static std::int32_t interp(std::int32_t y, std::int32_t ya, std::int32_t yb,
                               std::int32_t xa, std::int32_t xb) noexcept
    {
        if (yb == ya) return xa;
        return xa + (xb - xa) * (y - ya) / (yb - ya);
    }

    void fill_span(std::int32_t xl, std::int32_t xr, std::int32_t y, std::uint16_t c) noexcept
    {
        if (y < 0 || y >= h_) return;
        xl = std::max<std::int32_t>(0, xl);
        xr = std::min<std::int32_t>(w_ - 1, xr);
        std::uint16_t* row = buf_.data() + static_cast<std::size_t>(y) * w_;
        for (std::int32_t xx = xl; xx <= xr; ++xx) row[xx] = c;
    }

    void fill_corner(std::int32_t cx, std::int32_t cy, std::int32_t r, int sx, int sy,
                     std::uint16_t c) noexcept
    {
        const std::int32_t r2 = r * r;
        for (std::int32_t dy = 0; dy <= r; ++dy) {
            const std::int32_t dx = static_cast<std::int32_t>(std::sqrt(static_cast<double>(r2 - dy * dy) + 0.5));
            const std::int32_t y = cy + sy * dy;
            fill_span(cx, cx + sx * dx, y, c);
        }
    }

    std::int32_t w_{0};
    std::int32_t h_{0};
    std::vector<std::uint16_t> buf_;
};

// Display target — only a type is needed (we read the canvas buffer directly).
class M5GFX {
public:
    std::int32_t width() const noexcept { return 320; }
    std::int32_t height() const noexcept { return 240; }
};
