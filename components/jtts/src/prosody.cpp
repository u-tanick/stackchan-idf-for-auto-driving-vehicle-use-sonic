// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

// F0 輪郭のパラメータ。東京式アクセント句の「句頭で立ち上がり、句中は
// ゆるやかに下がり (declination)、文末で大きく落ちる」形の近似。単語ごとの
// アクセント核は辞書なしには分からないので、句レベルの形だけ与える。
// 平坦な F0 は「音の列」を「発話」として知覚させる手掛かりを欠くため、
// この形を与えるだけで聞き取りの負荷が下がる。
constexpr float kRiseMs = 130.0f;       // 句頭上昇の長さ
constexpr float kRiseStart = 0.90f;     // 立ち上がり開始の倍率
constexpr float kPeak = 1.05f;          // 上昇のピーク倍率
constexpr float kPreFall = 0.94f;       // 文末降下が始まる時点の倍率 (漸降の終点)
constexpr float kFallMs = 280.0f;       // 終端からこの時間で最終降下
constexpr float kFallRatio = 0.78f;     // 最終降下の相対倍率 (kPreFall に掛かる)

}  // namespace

ProsodyCurve::ProsodyCurve(float total_ms) : total_ms_(total_ms) {
    // 短い発話では区間をスケールする: 上昇は総長の 1/4 まで、最終降下は
    // 総長の 1/2 まで。1 モーラの発話でも rise→fall の山なり輪郭になる。
    rise_ms_ = std::min(kRiseMs, total_ms * 0.25f);
    fall_ms_ = std::min(kFallMs, total_ms * 0.50f);
    fall_start_ms_ = total_ms - fall_ms_;
}

float ProsodyCurve::at(float t_ms) const {
    if (total_ms_ <= 0.0f) return 1.0f;
    if (t_ms < rise_ms_) {
        float u = t_ms / rise_ms_;
        return kRiseStart + (kPeak - kRiseStart) * u;
    }
    if (t_ms <= fall_start_ms_) {
        float span = fall_start_ms_ - rise_ms_;
        float u = (span > 0.0f) ? (t_ms - rise_ms_) / span : 1.0f;
        return kPeak + (kPreFall - kPeak) * u;
    }
    float u = (t_ms - fall_start_ms_) / fall_ms_;
    if (u > 1.0f) u = 1.0f;
    return kPreFall * (1.0f - u * (1.0f - kFallRatio));
}

void apply_prosody(std::vector<Segment>& segs, const Options& /*opt*/) {
    if (segs.empty()) return;

    float total_ms = 0.0f;
    for (const auto& s : segs) total_ms += s.duration_ms;
    if (total_ms <= 0.0f) return;

    const ProsodyCurve curve(total_ms);
    float cursor_ms = 0.0f;
    for (auto& seg : segs) {
        const float seg_start = cursor_ms;
        const float seg_end = cursor_ms + seg.duration_ms;
        cursor_ms = seg_end;
        seg.start.f0_hz *= curve.at(seg_start);
        seg.end.f0_hz *= curve.at(seg_end);
    }
}

}  // namespace stackchan::jtts::internal
