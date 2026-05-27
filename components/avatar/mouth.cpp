// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "mouth.hpp"

namespace stackchan::avatar::internal {

void draw_mouth(Canvas& canvas, const Mouth& mouth, const DrawContext& ctx,
                std::int16_t breath_offset_y)
{
    const std::uint16_t fg = ctx.palette.primary;
    const float open = ctx.mouth_open_ratio;
    const std::uint16_t h = mouth.min_height +
                            static_cast<std::uint16_t>(static_cast<float>(mouth.max_height - mouth.min_height) * open);
    const std::uint16_t w = mouth.min_width +
                            static_cast<std::uint16_t>(static_cast<float>(mouth.max_width - mouth.min_width) * (1.0f - open));
    const std::int16_t x = mouth.center_x - static_cast<std::int16_t>(w / 2);
    const std::int16_t y = mouth.center_y - static_cast<std::int16_t>(h / 2) +
                           static_cast<std::int16_t>(ctx.breath * 2.0f) + breath_offset_y;
    // Size-varying rectangle (rule 3): a group sized to the *maximum* mouth
    // extent (+ breath travel) clears that whole region to the background before
    // the current rect is drawn, so the direct strategy leaves no residue from a
    // previously larger mouth. The buffered strategy treats the group as a no-op.
    const int gx = mouth.center_x - static_cast<int>(mouth.max_width) / 2 - 1;
    const int gy = mouth.center_y - static_cast<int>(mouth.max_height) / 2 - 8;
    const int gw = static_cast<int>(mouth.max_width) + 2;
    const int gh = static_cast<int>(mouth.max_height) + 16;
    canvas.begin_group(gx, gy, gw, gh);
    canvas.fillRect(x, y, static_cast<std::int16_t>(w), static_cast<std::int16_t>(h), fg);
    canvas.end_group();
}

} // namespace stackchan::avatar::internal
