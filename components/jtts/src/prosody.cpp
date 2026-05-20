// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

// 文末降下のパラメータ。
// kFallMs: 終端からこの時間だけ遡って F0 を下げ始める。
// kFallRatio: 終端で base F0 のこの倍率まで下げる (1.0 = 平坦)。
constexpr float kFallMs = 280.0f;
constexpr float kFallRatio = 0.78f;

}  // namespace

void apply_prosody(std::vector<Segment>& segs, const Options& /*opt*/) {
    if (segs.empty()) return;

    float total_ms = 0.0f;
    for (const auto& s : segs) total_ms += s.duration_ms;

    const float fall_start_ms = total_ms - kFallMs;
    if (fall_start_ms <= 0.0f) {
        // 発話が短すぎて 280 ms 全部を「降下区間」に当ててもまだ短い。
        // この場合は全体を線形に降下させる (ratio: 1.0 → kFallRatio)。
        if (total_ms <= 0.0f) return;
        float cursor = 0.0f;
        for (auto& seg : segs) {
            float t0 = cursor / total_ms;
            float t1 = (cursor + seg.duration_ms) / total_ms;
            cursor += seg.duration_ms;
            float m0 = 1.0f - t0 * (1.0f - kFallRatio);
            float m1 = 1.0f - t1 * (1.0f - kFallRatio);
            seg.start.f0_hz *= m0;
            seg.end.f0_hz *= m1;
        }
        return;
    }

    auto multiplier_at = [&](float t_ms) {
        if (t_ms <= fall_start_ms) return 1.0f;
        float u = (t_ms - fall_start_ms) / kFallMs;
        if (u > 1.0f) u = 1.0f;
        return 1.0f - u * (1.0f - kFallRatio);
    };

    float cursor_ms = 0.0f;
    for (auto& seg : segs) {
        const float seg_start = cursor_ms;
        const float seg_end = cursor_ms + seg.duration_ms;
        cursor_ms = seg_end;
        if (seg_end <= fall_start_ms) continue;
        seg.start.f0_hz *= multiplier_at(seg_start);
        seg.end.f0_hz *= multiplier_at(seg_end);
    }
}

}  // namespace stackchan::jtts::internal
