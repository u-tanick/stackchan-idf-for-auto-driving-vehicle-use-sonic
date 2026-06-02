// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "board/led_strip.hpp"

namespace stackchan::board {

tl::expected<void, Error> LedStrip::begin()
{
    if (auto r = expander_->set_led_count(count_); !r) return r;
    clear();
    return show();
}

void LedStrip::clear() noexcept
{
    buf_.fill(0);
}

// PY32 stores byte triples in the WS2812 wire order — G, R, B — not RGB.
// Sending (R, G, B) made pure red come out green because the firmware was
// reading byte 0 as G. Use GRB throughout the I2C buffer; the public API
// keeps the natural (r, g, b) parameter order.
void LedStrip::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    for (std::size_t i = 0; i < count_; ++i) {
        buf_[i * 3 + 0] = g;
        buf_[i * 3 + 1] = r;
        buf_[i * 3 + 2] = b;
    }
}

void LedStrip::set(std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    if (index >= count_) return;
    buf_[index * 3 + 0] = g;
    buf_[index * 3 + 1] = r;
    buf_[index * 3 + 2] = b;
}

tl::expected<void, Error> LedStrip::show()
{
    if (auto r = expander_->write_led_colors(buf_.data(), count_); !r) return r;
    return expander_->refresh_leds();
}

} // namespace stackchan::board
