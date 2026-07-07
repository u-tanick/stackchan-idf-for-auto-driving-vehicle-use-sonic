// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// フォルマント合成 Classic バリアント: インパルス列励起 + 並列 BPF×3。
// a14bd75 以前の実装の保存版 — ブザー的なロボット声が好みのユーザー向けに
// jtts_config の "synth": "classic" で選べる。V2 専用パラメータ
// (glottal_oq / tilt_db) は無視する。bw_scale は有効。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "internal.hpp"
#include "synth_dsp.hpp"

namespace stackchan::jtts::internal {

void render_segments_classic(std::span<const Segment> segs, std::vector<std::int16_t>& out,
                             const Options& opt) {
    using namespace dsp;
    const float fs = static_cast<float>(opt.sample_rate_hz);
    const float bw_scale = (opt.bw_scale > 0.0f) ? opt.bw_scale : 1.0f;
    Biquad r1, r2, r3;
    ImpulseSource voice;
    std::mt19937 rng(0xC0DECAFEu);
    auto noise = [&]() {
        std::uint32_t v = rng();
        return (static_cast<float>(v) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    };

    constexpr float step_ms = 5.0f;
    const std::size_t step_samples =
        std::max<std::size_t>(1, static_cast<std::size_t>(step_ms * 0.001f * fs));

    const bool vibrato_on = opt.vibrato_rate_hz > 0.0f && opt.vibrato_cents > 0.0f;
    const float vib_inc = opt.vibrato_rate_hz / fs;
    float vib_phase = 0.0f;
    // ノイズは impulse train (RMS≈1) と振幅を揃えるため √3 倍する (一様乱数 [-1,1]
    // の RMS = 1/√3)。これで breathiness の知覚的ミックスが線形になる。
    constexpr float kNoiseGain = 1.73205081f;

    for (const Segment& seg : segs) {
        std::size_t total = static_cast<std::size_t>(std::lround(seg.duration_ms * 0.001f * fs));
        if (total == 0) continue;

        std::size_t k = 0;
        while (k < total) {
            std::size_t n = std::min(step_samples, total - k);
            float t = static_cast<float>(k + n / 2) / static_cast<float>(total);

            float f1 = lerp(seg.start.f1, seg.end.f1, t);
            float f2 = lerp(seg.start.f2, seg.end.f2, t);
            float f3 = lerp(seg.start.f3, seg.end.f3, t);
            float bw1 = lerp(seg.start.bw1, seg.end.bw1, t) * bw_scale;
            float bw2 = lerp(seg.start.bw2, seg.end.bw2, t) * bw_scale;
            float bw3 = lerp(seg.start.bw3, seg.end.bw3, t) * bw_scale;
            float a1 = lerp(seg.start.a1, seg.end.a1, t);
            float a2 = lerp(seg.start.a2, seg.end.a2, t);
            float a3 = lerp(seg.start.a3, seg.end.a3, t);
            float voicing = lerp(seg.start.voicing, seg.end.voicing, t);
            float frication = lerp(seg.start.frication, seg.end.frication, t);
            float f0_base = lerp(seg.start.f0_hz, seg.end.f0_hz, t);

            r1.set_bpf(f1, bw1, fs);
            r2.set_bpf(f2, bw2, fs);
            r3.set_bpf(f3, bw3, fs);

            for (std::size_t j = 0; j < n; ++j) {
                float f0_mod = f0_base;
                if (vibrato_on) {
                    vib_phase += vib_inc;
                    if (vib_phase >= 1.0f) vib_phase -= 1.0f;
                    float lfo = std::sin(2.0f * kPi * vib_phase);
                    f0_mod = f0_base * std::exp2(opt.vibrato_cents / 1200.0f * lfo);
                }

                float pulse = voice.tick(f0_mod, fs);
                float n1 = noise() * kNoiseGain;
                float n2 = noise() * kNoiseGain;
                float voiced_src = (1.0f - opt.breathiness) * pulse + opt.breathiness * n1;
                float ex = voicing * voiced_src * opt.voicing_mul +
                           frication * n2 * opt.frication_mul;
                float y = a1 * r1.process(ex) + a2 * r2.process(ex) + a3 * r3.process(ex);
                y *= opt.gain;
                out.push_back(to_i16(y));
            }
            k += n;
        }
    }
}

}  // namespace stackchan::jtts::internal
