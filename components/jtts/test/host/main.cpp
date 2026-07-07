// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// jtts_synth_demo: ひらがな (UTF-8) を引数で受け取り WAV ファイルに書き出す CLI。
//
// Usage:
//   jtts_synth_demo "こんにちは" hello.wav [preset] [f0_hz]
//
// preset:
//   m         男性 (デフォルト)
//   f         女性
//   child     子供っぽい (高ピッチ + 短い声道)
//   elderly   老人 (息混じり + ゆっくり震え)
//   whisper   囁き声 (男声ベース、無声化)
//   whisper-f 囁き声 (女声ベース)
//   breathy   息混じり (男声)
//   breathy-f 息混じり (女声)
//   tremor    軽い震え声 (歌う寸前の声)
//   soft / soft-f       やわらかめ (OQ 0.70 + tilt 9 dB + 帯域幅 1.3)
//   classic / classic-f 旧方式 (インパルス + 並列 BPF)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "jtts/jtts.hpp"
#include "wav_writer.hpp"

namespace {

std::u32string utf8_to_u32(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        std::uint8_t c = static_cast<std::uint8_t>(utf8[i]);
        char32_t cp = 0;
        std::size_t n = 0;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            n = 4;
        } else {
            ++i;
            continue;
        }
        if (i + n > utf8.size()) break;
        for (std::size_t k = 1; k < n; ++k) {
            cp = (cp << 6) | (static_cast<std::uint8_t>(utf8[i + k]) & 0x3F);
        }
        out.push_back(cp);
        i += n;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <kana-utf8> <output.wav> [m|f] [f0_hz]\n", argv[0]);
        return 1;
    }
    std::string kana_utf8 = argv[1];
    std::string out_path = argv[2];

    std::u32string kana = utf8_to_u32(kana_utf8);
    std::vector<std::int16_t> pcm;

    stackchan::jtts::Options opt;
    if (argc >= 4) {
        using V = stackchan::jtts::Voice;
        std::string p = argv[3];
        if (p == "m" || p == "male") {
            opt.voice = V::Male;
        } else if (p == "f" || p == "female") {
            opt.voice = V::Female;
        } else if (p == "child") {
            opt.voice = V::Female;
            opt.f0_hz = 280.0f;
            opt.formant_scale = 1.30f;
            opt.mora_ms = 120.0f;
        } else if (p == "elderly") {
            opt.voice = V::Male;
            opt.f0_hz = 115.0f;
            opt.formant_scale = 1.03f;
            opt.breathiness = 0.35f;
            opt.vibrato_rate_hz = 4.5f;
            opt.vibrato_cents = 40.0f;
            opt.mora_ms = 130.0f;
        } else if (p == "whisper") {
            opt.voice = V::Male;
            opt.breathiness = 1.0f;
            opt.voicing_mul = 0.8f;
            opt.gain = 0.7f;
        } else if (p == "whisper-f") {
            opt.voice = V::Female;
            opt.breathiness = 1.0f;
            opt.voicing_mul = 0.8f;
            opt.gain = 0.7f;
        } else if (p == "breathy") {
            opt.voice = V::Male;
            opt.breathiness = 0.45f;
        } else if (p == "breathy-f") {
            opt.voice = V::Female;
            opt.breathiness = 0.45f;
        } else if (p == "tremor") {
            opt.vibrato_rate_hz = 5.0f;
            opt.vibrato_cents = 25.0f;
        } else if (p == "soft" || p == "soft-f") {
            // やわらかめ推奨値のショーケース (Options のコメント参照)。
            opt.voice = (p == "soft") ? V::Male : V::Female;
            opt.glottal_oq = 0.70f;
            opt.tilt_db = 9.0f;
            opt.bw_scale = 1.3f;
            opt.breathiness = 0.15f;
        } else if (p == "classic" || p == "classic-f") {
            opt.voice = (p == "classic") ? V::Male : V::Female;
            opt.synth = stackchan::jtts::SynthVariant::Classic;
        } else {
            std::fprintf(stderr, "Unknown preset: %s\n", p.c_str());
            return 1;
        }
    }
    if (argc >= 5) {
        opt.f0_hz = static_cast<float>(std::atof(argv[4]));
    }
    auto r = stackchan::jtts::synthesize(kana, pcm, opt);
    if (!r) {
        std::fprintf(stderr, "synthesize failed: %s\n", stackchan::jtts::to_string(r.error()));
        return 2;
    }
    if (!write_wav_mono16(out_path, pcm, opt.sample_rate_hz)) {
        std::fprintf(stderr, "wav write failed: %s\n", out_path.c_str());
        return 3;
    }
    std::printf("wrote %s (%zu samples, %.3f s)\n", out_path.c_str(), pcm.size(),
                static_cast<double>(pcm.size()) / opt.sample_rate_hz);
    return 0;
}
