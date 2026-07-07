// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// jtts_unit_demo: 単位連結 (PSOLA) エンジンでひらがなを WAV にする CLI。
//
// Usage: jtts_unit_demo <units.jvox> "<kana-utf8>" <out.wav> [f0_hz] [mora_ms]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "internal.hpp"
#include "jtts/jvox.hpp"
#include "wav_writer.hpp"

using namespace stackchan::jtts;

namespace {

std::u32string utf8_to_u32(std::string_view utf8) {
    std::u32string out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        std::uint8_t c = static_cast<std::uint8_t>(utf8[i]);
        char32_t cp = 0;
        std::size_t n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { ++i; continue; }
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
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <units.jvox> <kana-utf8> <out.wav> [f0_hz] [mora_ms]\n",
                     argv[0]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<std::uint8_t> blob{std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>()};
    auto db = jvox::Db::parse(blob);
    if (!db) {
        std::fprintf(stderr, "jvox parse failed\n");
        return 1;
    }

    Options opt;
    opt.sample_rate_hz = db->sample_rate();
    opt.f0_hz = (argc >= 5) ? static_cast<float>(std::atof(argv[4])) : 210.0f;
    opt.mora_ms = (argc >= 6) ? static_cast<float>(std::atof(argv[5])) : 130.0f;
    opt.gain = 0.8f;

    std::vector<Mora> moras;
    if (!internal::parse_kana(utf8_to_u32(argv[2]), moras)) {
        std::fprintf(stderr, "kana parse failed\n");
        return 1;
    }
    internal::apply_devoicing(moras);

    std::vector<std::int16_t> pcm;
    if (!internal::render_units(moras, *db, pcm, opt)) {
        std::fprintf(stderr, "render_units failed (missing units?)\n");
        return 2;
    }
    if (!write_wav_mono16(argv[3], pcm, opt.sample_rate_hz)) {
        std::fprintf(stderr, "wav write failed\n");
        return 3;
    }
    std::printf("wrote %s (%zu samples, %.3f s)\n", argv[3], pcm.size(),
                static_cast<double>(pcm.size()) / opt.sample_rate_hz);
    return 0;
}
