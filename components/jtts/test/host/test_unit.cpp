// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// 単位連結 + TD-PSOLA エンジンの自動検証。ブートストラップ .jvox
// (gen_units + pack_jvox.py 製) を読み込み、以下を確認する:
//   1. 目標 F0 追従: 2 種類の F0 で合成し、自己相関トラックの中央値が
//      目標 (×韻律中央値 ≈0.97) の ±12% に入る
//   2. 時間長: 合成長がモーラ数 × mora_ms の ±15% に入る
//   3. 連結クリック: 隣接サンプル差の最大値が全振幅の 60% を超えない
//
// Usage: jtts_test_unit <units.jvox> [outdir]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jvox.hpp"
#include "wav_writer.hpp"

using namespace stackchan::jtts;

namespace {

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// 30 ms フレームの自己相関 F0 トラック → 有声フレームの中央値。
float median_f0(const std::vector<std::int16_t>& x, float fs) {
    const std::size_t n = static_cast<std::size_t>(fs * 0.030f);
    std::vector<float> f0s;
    for (std::size_t i = 0; i + n < x.size(); i += n) {
        double e0 = 0;
        for (std::size_t k = 0; k < n; ++k) {
            e0 += static_cast<double>(x[i + k]) * x[i + k];
        }
        if (e0 < 1e6) continue;
        const std::size_t lag_lo = static_cast<std::size_t>(fs / 400.0f);
        const std::size_t lag_hi = static_cast<std::size_t>(fs / 80.0f);
        double best = 0;
        std::size_t best_lag = 0;
        for (std::size_t lag = lag_lo; lag < lag_hi && i + n + lag < x.size(); ++lag) {
            double ac = 0;
            for (std::size_t k = 0; k < n; ++k) {
                ac += static_cast<double>(x[i + k]) * x[i + k + lag];
            }
            if (ac > best) {
                best = ac;
                best_lag = lag;
            }
        }
        if (best_lag > 0 && best > 0.30 * e0) {
            f0s.push_back(fs / static_cast<float>(best_lag));
        }
    }
    if (f0s.empty()) return 0.0f;
    std::sort(f0s.begin(), f0s.end());
    return f0s[f0s.size() / 2];
}

int g_failures = 0;

void check(bool ok, const char* what, float got, float want_lo, float want_hi) {
    std::printf("[%s] %-28s got=%8.1f want=[%.1f, %.1f]\n", ok ? " OK " : "FAIL", what,
                static_cast<double>(got), static_cast<double>(want_lo),
                static_cast<double>(want_hi));
    if (!ok) ++g_failures;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <units.jvox> [outdir]\n", argv[0]);
        return 1;
    }
    const auto blob = read_file(argv[1]);
    auto db = jvox::Db::parse(blob);
    if (!db) {
        std::fprintf(stderr, "jvox parse failed: %s\n", argv[1]);
        return 1;
    }
    std::printf("db: %u units, fs=%u\n", db->unit_count(), db->sample_rate());
    const std::string outdir = (argc >= 3) ? argv[2] : ".";

    const std::u32string text = U"こんにちは、すたっくちゃんです";
    std::vector<Mora> moras;
    internal::parse_kana(text, moras);
    internal::apply_devoicing(moras);

    for (float f0 : {140.0f, 220.0f}) {
        Options opt;
        opt.sample_rate_hz = db->sample_rate();
        opt.f0_hz = f0;
        opt.mora_ms = 130.0f;
        opt.gain = 0.8f;

        std::vector<std::int16_t> pcm;
        if (!internal::render_units(moras, *db, pcm, opt)) {
            std::printf("[FAIL] render_units returned false (missing units?)\n");
            ++g_failures;
            continue;
        }

        char name[64];
        std::snprintf(name, sizeof(name), "%s/unit_f0_%d.wav", outdir.c_str(),
                      static_cast<int>(f0));
        write_wav_mono16(name, pcm, opt.sample_rate_hz);

        // 1. F0 追従 (韻律輪郭の中央値 ≈0.97 を織り込む)
        const float med = median_f0(pcm, static_cast<float>(opt.sample_rate_hz));
        check(med > f0 * 0.97f * 0.88f && med < f0 * 0.97f * 1.12f, "median F0", med,
              f0 * 0.97f * 0.88f, f0 * 0.97f * 1.12f);

        // 2. 時間長 (モーラ数 × mora_ms、っ の 70ms ギャップ等の誤差込み ±15%)
        std::size_t cv_count = 0;
        for (const auto& m : moras) {
            if (m.kind != MoraKind::Sokuon) ++cv_count;
        }
        const float expect_ms = static_cast<float>(cv_count) * opt.mora_ms;
        const float got_ms =
            static_cast<float>(pcm.size()) * 1000.0f / static_cast<float>(opt.sample_rate_hz);
        check(got_ms > expect_ms * 0.85f && got_ms < expect_ms * 1.15f, "total duration ms",
              got_ms, expect_ms * 0.85f, expect_ms * 1.15f);

        // 3. 連結クリック (隣接サンプル差)
        std::int32_t max_jump = 0;
        for (std::size_t i = 1; i < pcm.size(); ++i) {
            const std::int32_t d = std::abs(static_cast<std::int32_t>(pcm[i]) - pcm[i - 1]);
            if (d > max_jump) max_jump = d;
        }
        check(max_jump < 39000, "max sample jump", static_cast<float>(max_jump), 0.0f, 39000.0f);
        std::printf("  f0=%d: %zu samples -> %s\n", static_cast<int>(f0), pcm.size(), name);
    }

    if (g_failures == 0) {
        std::printf("all unit-engine checks passed\n");
        return 0;
    }
    std::printf("%d check(s) FAILED\n", g_failures);
    return 1;
}
