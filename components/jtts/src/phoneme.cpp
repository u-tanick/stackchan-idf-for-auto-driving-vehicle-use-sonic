// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>

#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

FormantFrame silent_frame(float f0) {
    FormantFrame s;
    s.voicing = 0.0f;
    s.frication = 0.0f;
    s.a1 = s.a2 = s.a3 = 0.0f;
    s.f0_hz = f0;
    return s;
}

void add_cv_segments(Consonant c, Vowel v, bool palatalized, bool devoiced, float mora_ms,
                     float f0, std::vector<Segment>& out) {
    FormantFrame vowel = vowel_frame(v, palatalized);
    vowel.f0_hz = f0;

    if (devoiced) {
        // 母音を囁き化: 声帯駆動を切り、ノイズで母音フォルマントを軽く励起する。
        // 振幅も下げて短く弱く感じさせる。
        vowel.voicing = 0.0f;
        vowel.frication = 0.35f;
        vowel.a1 *= 0.55f;
        vowel.a2 *= 0.55f;
        vowel.a3 *= 0.55f;
    }

    if (c == Consonant::None) {
        out.push_back({vowel, vowel, mora_ms});
        return;
    }

    FormantFrame burst = consonant_burst(c, v);
    burst.f0_hz = f0;
    if (palatalized && !is_nasal(c)) {
        burst.f2 += 200.0f;
    }

    const FormantFrame silence = silent_frame(f0);

    auto push_vowel_tail = [&](float consumed) {
        float v_ms = std::max(20.0f, mora_ms - consumed);
        out.push_back({vowel, vowel, v_ms});
    };

    if (is_voiceless_stop(c)) {
        out.push_back({silence, silence, 30.0f});
        out.push_back({burst, burst, 10.0f});
        out.push_back({burst, vowel, 30.0f});
        push_vowel_tail(70.0f);
    } else if (is_voiced_stop(c)) {
        out.push_back({burst, burst, 15.0f});
        out.push_back({burst, vowel, 30.0f});
        push_vowel_tail(45.0f);
    } else if (is_voiceless_fric(c)) {
        // 立ち上がりを 25 ms かけて滑らかに上げる。
        // これを入れないと /ʃ/ /s/ のノイズが突然全振幅で出て破擦音 /tʃ/
        // /ts/ と区別がつかなくなる (「した」が「ちた」に聞こえる現象)。
        FormantFrame burst_soft = burst;
        burst_soft.frication = 0.25f;
        burst_soft.a3 *= 0.40f;
        out.push_back({burst_soft, burst, 25.0f});
        out.push_back({burst, burst, 45.0f});
        out.push_back({burst, vowel, 20.0f});
        push_vowel_tail(90.0f);
    } else if (is_voiced_fric_affric(c)) {
        out.push_back({burst, burst, 50.0f});
        out.push_back({burst, vowel, 25.0f});
        push_vowel_tail(75.0f);
    } else if (is_affricate(c)) {
        out.push_back({silence, silence, 25.0f});
        out.push_back({burst, burst, 40.0f});
        out.push_back({burst, vowel, 20.0f});
        push_vowel_tail(85.0f);
    } else if (is_nasal(c)) {
        FormantFrame nf = nasal_frame(c);
        nf.f0_hz = f0;
        out.push_back({nf, nf, 50.0f});
        out.push_back({nf, vowel, 20.0f});
        push_vowel_tail(70.0f);
    } else if (is_glide(c)) {
        out.push_back({burst, vowel, 50.0f});
        push_vowel_tail(50.0f);
    } else if (c == Consonant::R) {
        out.push_back({burst, vowel, 25.0f});
        push_vowel_tail(25.0f);
    } else {
        out.push_back({vowel, vowel, mora_ms});
    }
}

}  // namespace

void build_segments(std::span<const Mora> moras, std::vector<Segment>& out, const Options& opt) {
    out.clear();
    const float f0 = opt.f0_hz;
    const float mora_ms = opt.mora_ms;

    for (std::size_t i = 0; i < moras.size(); ++i) {
        const Mora& m = moras[i];
        switch (m.kind) {
            case MoraKind::CV:
                add_cv_segments(m.c, m.v, m.palatalized, m.devoiced, mora_ms, f0, out);
                break;
            case MoraKind::MoraicN: {
                Consonant nasal_c = Consonant::N;
                if (i + 1 < moras.size() && moras[i + 1].kind == MoraKind::CV) {
                    Consonant nc = moras[i + 1].c;
                    if (nc == Consonant::M || nc == Consonant::B || nc == Consonant::P) {
                        nasal_c = Consonant::M;
                    }
                }
                FormantFrame nf = nasal_frame(nasal_c);
                nf.f0_hz = f0;
                out.push_back({nf, nf, mora_ms});
                break;
            }
            case MoraKind::Sokuon: {
                FormantFrame s = silent_frame(f0);
                out.push_back({s, s, 70.0f});
                break;
            }
            case MoraKind::Chouon: {
                if (!out.empty()) {
                    FormantFrame ref = out.back().end;
                    out.push_back({ref, ref, mora_ms});
                }
                break;
            }
        }
    }
}

void build_segments(std::span<const Mora>, std::span<Segment>) {}

void apply_devoicing(std::vector<Mora>& moras) {
    for (std::size_t i = 0; i < moras.size(); ++i) {
        Mora& m = moras[i];
        if (m.kind != MoraKind::CV) continue;
        if (m.v != Vowel::I && m.v != Vowel::U) continue;
        if (!is_voiceless_consonant(m.c)) continue;

        // 「次が無声」かどうかを判定。CV なら子音、Sokuon (っ) は無声子音前置の
        // マーカなので常に無声扱い、Chouon (長音) は母音延長なので有声扱い、
        // MoraicN (ん) は有声扱い、末尾 (i+1==N) は文末でやはり無声化が起こる。
        bool next_voiceless = false;
        if (i + 1 == moras.size()) {
            next_voiceless = true;
        } else {
            const Mora& nx = moras[i + 1];
            switch (nx.kind) {
                case MoraKind::CV:
                    next_voiceless = is_voiceless_consonant(nx.c);
                    break;
                case MoraKind::Sokuon:
                    next_voiceless = true;
                    break;
                case MoraKind::Chouon:
                case MoraKind::MoraicN:
                    next_voiceless = false;
                    break;
            }
        }

        if (next_voiceless) {
            m.devoiced = true;
        }
    }
}

}  // namespace stackchan::jtts::internal
