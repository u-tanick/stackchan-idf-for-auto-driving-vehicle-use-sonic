// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// gen_units: フォルマント合成でモーラ単位 WAV をブートストラップ生成する。
// 単位連結 (PSOLA) エンジンのアルゴリズム検証用 — 実音声データのライセンスと
// 切り離して、パイプライン (pack_jvox.py → unit_synth) 全体をテストできる。
// 品質向上が目的ではない (それは実音声 DB で置き換える)。
//
// Usage: jtts_gen_units <outdir>
//   outdir/mora_<key hex>.wav (16 kHz i16 mono) と outdir/index.tsv
//   (key \t kana \t filename) を書き出す。
#include <cstdio>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jvox.hpp"
#include "wav_writer.hpp"

using namespace stackchan::jtts;

namespace {

// kana_table が受理する全モーラ。拗音は「Iモーラ + ゃゅょ」で parse される。
const char32_t* kMoraKana[] = {
    U"あ", U"い", U"う", U"え", U"お",
    U"か", U"き", U"く", U"け", U"こ", U"が", U"ぎ", U"ぐ", U"げ", U"ご",
    U"さ", U"し", U"す", U"せ", U"そ", U"ざ", U"じ", U"ず", U"ぜ", U"ぞ",
    U"た", U"ち", U"つ", U"て", U"と", U"だ", U"で", U"ど",
    U"な", U"に", U"ぬ", U"ね", U"の",
    U"は", U"ひ", U"ふ", U"へ", U"ほ", U"ば", U"び", U"ぶ", U"べ", U"ぼ",
    U"ぱ", U"ぴ", U"ぷ", U"ぺ", U"ぽ",
    U"ま", U"み", U"む", U"め", U"も",
    U"や", U"ゆ", U"よ",
    U"ら", U"り", U"る", U"れ", U"ろ",
    U"わ", U"を",
    U"きゃ", U"きゅ", U"きょ", U"ぎゃ", U"ぎゅ", U"ぎょ",
    U"しゃ", U"しゅ", U"しょ", U"じゃ", U"じゅ", U"じょ",
    U"ちゃ", U"ちゅ", U"ちょ",
    U"にゃ", U"にゅ", U"にょ",
    U"ひゃ", U"ひゅ", U"ひょ", U"びゃ", U"びゅ", U"びょ", U"ぴゃ", U"ぴゅ", U"ぴょ",
    U"みゃ", U"みゅ", U"みょ",
    U"りゃ", U"りゅ", U"りょ",
    U"ん",
};

std::string u32_to_utf8(const char32_t* s) {
    std::string out;
    for (; *s; ++s) {
        char32_t cp = *s;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <outdir>\n", argv[0]);
        return 1;
    }
    const std::string outdir = argv[1];

    // 単位収録の条件: 韻律・無声化を掛けず、F0 一定・十分な長さで合成する
    // (PSOLA が伸縮する余地を残すため mora_ms は長めに取る)。
    Options opt;
    opt.sample_rate_hz = 16000;
    opt.f0_hz = 130.0f;
    opt.formant_scale = 1.0f;
    opt.mora_ms = 200.0f;
    opt.gain = 0.6f;

    const std::string index_path = outdir + "/index.tsv";
    std::FILE* index = std::fopen(index_path.c_str(), "w");
    if (index == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", index_path.c_str());
        return 1;
    }

    int written = 0;
    for (const char32_t* kana : kMoraKana) {
        std::vector<Mora> moras;
        if (!internal::parse_kana(std::u32string(kana), moras) || moras.size() != 1) {
            std::fprintf(stderr, "skip (parse): %s\n", u32_to_utf8(kana).c_str());
            continue;
        }
        const Mora& m = moras[0];
        const std::uint16_t key =
            (m.kind == MoraKind::MoraicN) ? jvox::kMoraicNKey : jvox::unit_key(m);

        std::vector<internal::Segment> segs;
        internal::build_segments(moras, segs, opt);
        if (segs.empty()) continue;
        std::vector<std::int16_t> pcm;
        internal::render_segments(segs, pcm, opt);

        char name[32];
        std::snprintf(name, sizeof(name), "mora_%04x.wav", key);
        const std::string path = outdir + "/" + name;
        if (!write_wav_mono16(path, pcm, opt.sample_rate_hz)) {
            std::fprintf(stderr, "wav write failed: %s\n", path.c_str());
            std::fclose(index);
            return 1;
        }
        std::fprintf(index, "%04x\t%s\t%s\t%.1f\n", key, u32_to_utf8(kana).c_str(), name,
                     static_cast<double>(opt.f0_hz));
        ++written;
    }
    std::fclose(index);
    std::printf("wrote %d units + index.tsv to %s\n", written, outdir.c_str());
    return 0;
}
