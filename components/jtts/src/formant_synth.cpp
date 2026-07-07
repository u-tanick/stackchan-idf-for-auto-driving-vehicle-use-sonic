// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// フォルマント合成 V2: Rosenberg 声門波励起 + Klatt 共振器カスケード。
// Classic バリアント (インパルス + 並列) は formant_synth_classic.cpp。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "internal.hpp"
#include "synth_dsp.hpp"

namespace stackchan::jtts::internal {

namespace {

using namespace dsp;

void render_v2(std::span<const Segment> segs, std::vector<std::int16_t>& out,
               const Options& opt) {
    const float fs = static_cast<float>(opt.sample_rate_hz);
    const float bw_scale = (opt.bw_scale > 0.0f) ? opt.bw_scale : 1.0f;

    // 有声パス: 声門波 → tilt → R1→R2→R3→R4→R5 カスケード。
    Resonator c1, c2, c3, c4, c5;
    // 無声パス: ノイズ → 並列 BPF ×3 (a1..a3 で振幅制御)。
    Biquad p1, p2, p3;
    GlottalSource voice;
    voice.set_oq(opt.glottal_oq);
    TiltFilter tilt;
    tilt.set(opt.tilt_db, fs);
    std::mt19937 rng(0xC0DECAFEu);
    auto noise = [&]() {
        std::uint32_t v = rng();
        return (static_cast<float>(v) / static_cast<float>(0xFFFFFFFFu)) * 2.0f - 1.0f;
    };

    // F4/F5 は母音間でほとんど動かないので固定共振器として置く。3 フォルマント
    // では 3 kHz 以上がごっそり欠けて「こもった電話声」になるのを埋める。
    // 声道長の違い (formant_scale) には追従させる。
    const float fscale = (opt.formant_scale > 0.0f) ? opt.formant_scale : 1.0f;
    c4.set(3300.0f * fscale, 280.0f * bw_scale, fs);
    c5.set(3850.0f * fscale, 320.0f * bw_scale, fs);

    constexpr float step_ms = 5.0f;
    const std::size_t step_samples =
        std::max<std::size_t>(1, static_cast<std::size_t>(step_ms * 0.001f * fs));

    const bool vibrato_on = opt.vibrato_rate_hz > 0.0f && opt.vibrato_cents > 0.0f;
    const float vib_inc = opt.vibrato_rate_hz / fs;
    float vib_phase = 0.0f;
    // ノイズ (一様乱数 [-1,1]、RMS = 1/√3) を声門波の実効振幅と揃えるための係数。
    constexpr float kNoiseGain = 1.73205081f;
    // カスケード出力の全体ゲイン。声門波振幅 (≈π/2Tn) とカスケードの帯域圧縮を
    // 込みで、Classic (並列 + インパルス) と同程度の出力 RMS になるよう
    // ホストの母音テスト (test_vowels) で合わせた値。
    constexpr float kVoicedMakeup = 0.015f;

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

            // カスケード係数はブロック長かけて補間 (係数ステップの広帯域
            // 過渡が tilt の効きを覆い隠すのを防ぐ — Resonator のコメント参照)。
            c1.set_target(f1, bw1, fs, static_cast<int>(n));
            c2.set_target(f2, bw2, fs, static_cast<int>(n));
            c3.set_target(f3, bw3, fs, static_cast<int>(n));
            p1.set_bpf(f1, bw1, fs);
            p2.set_bpf(f2, bw2, fs);
            p3.set_bpf(f3, bw3, fs);

            // カスケードは相対振幅を構造が決めるので、フォルマント別振幅の
            // 代わりに a1 を「その区間の有声マスター音量」として使う (母音 1.0、
            // 鼻音 0.55、無声化母音 0.55 などテーブルの意図はそのまま活きる)。
            const float voiced_amp = a1;

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
                // tilt はノイズ混合の後: 気息成分も一緒にやわらかくなる。
                float voiced_src =
                    tilt.process((1.0f - opt.breathiness) * pulse + opt.breathiness * n1);

                float voiced =
                    c5.process(c4.process(c3.process(c2.process(c1.process(voiced_src)))));
                voiced *= voicing * voiced_amp * opt.voicing_mul * kVoicedMakeup;

                float fric_src = frication * n2 * opt.frication_mul;
                float fric = a1 * p1.process(fric_src) + a2 * p2.process(fric_src) +
                             a3 * p3.process(fric_src);

                float y = (voiced + fric) * opt.gain;
                out.push_back(to_i16(y));
            }
            k += n;
        }
    }
}

}  // namespace

void render_segments(std::span<const Segment> segs, std::vector<std::int16_t>& out,
                     const Options& opt) {
    if (opt.synth == SynthVariant::Classic) {
        render_segments_classic(segs, out, opt);
        return;
    }
    render_v2(segs, out, opt);
}

}  // namespace stackchan::jtts::internal
