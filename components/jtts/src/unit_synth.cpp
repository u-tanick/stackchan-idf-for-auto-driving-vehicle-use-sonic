// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// モーラ単位連結 + TD-PSOLA エンジン (案 B)。
// .jvox の単位波形 (実音声 or ブートストラップ合成) を連結し、ピッチマーク
// 同期の overlap-add で目標 F0・時間長に変形する。フォルマント合成より
// CPU が軽く (窓掛けコピーと加算のみ)、実音声由来のスペクトル微細構造が
// 残るぶん了解性が高い。
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "internal.hpp"
#include "jtts/jvox.hpp"

namespace stackchan::jtts::internal {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// モーラ列を「レンダリング計画」に落とす。Chouon は直前単位の時間伸長、
// Sokuon は無音として直前エントリの後置ギャップにする。
struct PlanEntry {
    jvox::UnitView unit;
    float duration_ms = 0.0f;   // 単位が埋めるべき長さ
    float gap_ms = 0.0f;        // 単位の後ろに置く無音 (っ)
    float amp = 1.0f;           // 無声化母音などの減衰
};

// 1 単位を PSOLA で target_len サンプルに変形して out に追記する。
// 単位構造: [先頭 .. marks[0]) = 無声オンセット (子音)、そのままコピー。
//           [marks[0] .. marks[last]] = 有声部、PSOLA で伸縮 + ピッチ変更。
//           (marks[last] .. 末尾] = 残差テール、そのままコピー。
// f0_at(t_out_samples) は出力サンプル位置 → 目標 F0 [Hz]。
template <typename F0Fn>
void render_unit_psola(const jvox::UnitView& u, std::size_t target_len, float fs, float amp,
                       F0Fn&& f0_at, std::vector<float>& out) {
    const std::size_t base = out.size();
    out.resize(base + target_len, 0.0f);
    if (u.pcm.empty() || target_len == 0) return;

    auto copy_verbatim = [&](std::size_t src0, std::size_t dst0, std::size_t n) {
        n = std::min({n, u.pcm.size() - src0, target_len - dst0});
        for (std::size_t i = 0; i < n; ++i) {
            out[base + dst0 + i] += amp * static_cast<float>(u.pcm[src0 + i]) / 32768.0f;
        }
    };

    if (u.marks.size() < 3) {
        // ピッチマークなし (無声単位): 等速コピー、長さは短い方に切る。
        copy_verbatim(0, 0, std::min(u.pcm.size(), target_len));
        return;
    }

    const std::size_t v_in0 = u.marks.front();
    const std::size_t v_in1 = u.marks.back();
    const std::size_t head_len = std::min<std::size_t>(v_in0, target_len);
    const std::size_t tail_in = u.pcm.size() - 1 - v_in1;  // 有声部より後ろ

    // 出力配分: head は等速、tail は最大でも入力と同じ長さ、残りが有声部。
    const std::size_t tail_len = std::min(tail_in, target_len - head_len);
    const std::size_t v_out_len =
        (target_len > head_len + tail_len) ? target_len - head_len - tail_len : 0;

    copy_verbatim(0, 0, head_len);

    if (v_out_len > 0) {
        const float v_in_len = static_cast<float>(v_in1 - v_in0);
        // 合成マークを目標 F0 間隔で置き、対応する分析マークの 2 周期 Hann 窓
        // グレインを overlap-add する。時間軸は有声部を線形に対応付ける。
        float s = 0.0f;  // 有声部内の出力位置
        while (s < static_cast<float>(v_out_len)) {
            const std::size_t out_pos = head_len + static_cast<std::size_t>(s);
            float f0 = f0_at(base + out_pos);
            if (f0 < 40.0f) f0 = 40.0f;
            const float syn_period = fs / f0;

            // 分析位置: 有声部の進捗を入力へ写像し、最寄りの分析マークを選ぶ。
            const float uu = static_cast<float>(v_in0) +
                             (s / static_cast<float>(v_out_len)) * v_in_len;
            std::size_t j = 1;
            while (j + 1 < u.marks.size() && static_cast<float>(u.marks[j]) < uu) ++j;
            const std::size_t m = u.marks[j];
            const std::size_t p_prev = u.marks[j] - u.marks[j - 1];
            const std::size_t p_next =
                (j + 1 < u.marks.size()) ? u.marks[j + 1] - u.marks[j] : p_prev;
            const std::size_t p_a = std::max<std::size_t>(8, std::min(p_prev, p_next));

            // ピッチを上げる (合成間隔 < 分析周期) と窓の重なりが増えて音量が
            // 上がるので、グレインを間隔比で補正する。
            float gain = syn_period / static_cast<float>(p_a);
            gain = std::clamp(gain, 0.5f, 2.0f) * amp;

            // 2 周期 Hann 窓グレイン (中心 = 分析マーク) を合成位置へ加算。
            for (std::size_t k = 0; k < 2 * p_a + 1; ++k) {
                const std::ptrdiff_t src =
                    static_cast<std::ptrdiff_t>(m + k) - static_cast<std::ptrdiff_t>(p_a);
                if (src < 0 || src >= static_cast<std::ptrdiff_t>(u.pcm.size())) continue;
                const std::ptrdiff_t dst = static_cast<std::ptrdiff_t>(out_pos + k) -
                                           static_cast<std::ptrdiff_t>(p_a);
                if (dst < static_cast<std::ptrdiff_t>(head_len) ||
                    dst >= static_cast<std::ptrdiff_t>(target_len)) {
                    continue;
                }
                const float w =
                    0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(k) /
                                           static_cast<float>(2 * p_a));
                out[base + dst] +=
                    gain * w * static_cast<float>(u.pcm[static_cast<std::size_t>(src)]) / 32768.0f;
            }
            s += syn_period;
        }
    }

    if (tail_len > 0) {
        copy_verbatim(v_in1 + 1, head_len + v_out_len, tail_len);
    }
}

}  // namespace

bool render_units(std::span<const Mora> moras, const jvox::Db& db,
                  std::vector<std::int16_t>& out, const Options& opt) {
    const float fs = static_cast<float>(opt.sample_rate_hz);

    // レンダリング計画を作りつつ、必要な単位が全部あるか先に確かめる
    // (欠けがあれば false を返して呼び出し側がフォルマント合成へ落とす)。
    std::vector<PlanEntry> plan;
    plan.reserve(moras.size());
    for (const Mora& m : moras) {
        switch (m.kind) {
            case MoraKind::CV: {
                auto u = db.find(jvox::unit_key(m));
                if (!u) return false;
                PlanEntry e;
                e.unit = *u;
                e.duration_ms = opt.mora_ms;
                if (m.devoiced) {
                    e.amp = 0.5f;
                    e.duration_ms *= 0.8f;
                }
                plan.push_back(e);
                break;
            }
            case MoraKind::MoraicN: {
                auto u = db.find(jvox::kMoraicNKey);
                if (!u) return false;
                PlanEntry e;
                e.unit = *u;
                e.duration_ms = opt.mora_ms;
                plan.push_back(e);
                break;
            }
            case MoraKind::Sokuon:
                if (!plan.empty()) {
                    plan.back().gap_ms += 70.0f;
                } // 文頭の っ は無視 (現行エンジンの silent 挙動と同等)
                break;
            case MoraKind::Chouon:
                if (!plan.empty()) {
                    plan.back().duration_ms += opt.mora_ms;
                }
                break;
        }
    }
    if (plan.empty()) return false;

    float total_ms = 0.0f;
    for (const auto& e : plan) total_ms += e.duration_ms + e.gap_ms;

    // 韻律: フォルマント合成と同じ句輪郭 (句頭上昇 → 漸降 → 文末降下) を
    // 目標 F0 に使う。
    const ProsodyCurve curve(total_ms);
    const float base_f0 = opt.f0_hz;
    auto f0_at_sample = [&](std::size_t abs_sample) {
        const float t_ms = static_cast<float>(abs_sample) * 1000.0f / fs;
        return base_f0 * curve.at(t_ms);
    };

    std::vector<float> mix;
    mix.reserve(static_cast<std::size_t>(total_ms * 0.001f * fs) + 64);
    for (const auto& e : plan) {
        const std::size_t n = static_cast<std::size_t>(std::lround(e.duration_ms * 0.001f * fs));
        render_unit_psola(e.unit, n, fs, e.amp, f0_at_sample, mix);
        const std::size_t gap = static_cast<std::size_t>(std::lround(e.gap_ms * 0.001f * fs));
        mix.resize(mix.size() + gap, 0.0f);
    }

    // 連結点のクリック除去: 全体に短いフェードイン/アウトだけ掛け、単位間は
    // PSOLA 窓が自然に重なるのに任せる (境界検査はホスト テストで監視)。
    const std::size_t fade = std::min<std::size_t>(mix.size() / 4, 32);
    for (std::size_t i = 0; i < fade; ++i) {
        const float w = static_cast<float>(i) / static_cast<float>(fade);
        mix[i] *= w;
        mix[mix.size() - 1 - i] *= w;
    }

    out.reserve(out.size() + mix.size());
    for (float v : mix) {
        float y = v * opt.gain;
        y = std::clamp(y, -1.0f, 1.0f);
        out.push_back(static_cast<std::int16_t>(y * 32760.0f));
    }
    return true;
}

}  // namespace stackchan::jtts::internal
