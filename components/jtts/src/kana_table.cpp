// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "internal.hpp"

namespace stackchan::jtts::internal {

namespace {

bool basic_mora(char32_t ch, Mora& mora) {
    using V = Vowel;
    using C = Consonant;
    auto cv = [&](C c, V v) {
        mora = Mora{MoraKind::CV, c, v, false};
        return true;
    };
    switch (ch) {
        case U'あ': return cv(C::None, V::A);
        case U'い': return cv(C::None, V::I);
        case U'う': return cv(C::None, V::U);
        case U'え': return cv(C::None, V::E);
        case U'お': return cv(C::None, V::O);
        case U'か': return cv(C::K, V::A);
        case U'き': return cv(C::K, V::I);
        case U'く': return cv(C::K, V::U);
        case U'け': return cv(C::K, V::E);
        case U'こ': return cv(C::K, V::O);
        case U'が': return cv(C::G, V::A);
        case U'ぎ': return cv(C::G, V::I);
        case U'ぐ': return cv(C::G, V::U);
        case U'げ': return cv(C::G, V::E);
        case U'ご': return cv(C::G, V::O);
        case U'さ': return cv(C::S, V::A);
        case U'し': return cv(C::Sh, V::I);
        case U'す': return cv(C::S, V::U);
        case U'せ': return cv(C::S, V::E);
        case U'そ': return cv(C::S, V::O);
        case U'ざ': return cv(C::Z, V::A);
        case U'じ': return cv(C::J, V::I);
        case U'ず': return cv(C::Z, V::U);
        case U'ぜ': return cv(C::Z, V::E);
        case U'ぞ': return cv(C::Z, V::O);
        case U'た': return cv(C::T, V::A);
        case U'ち': return cv(C::Ch, V::I);
        case U'つ': return cv(C::Ts, V::U);
        case U'て': return cv(C::T, V::E);
        case U'と': return cv(C::T, V::O);
        case U'だ': return cv(C::D, V::A);
        case U'ぢ': return cv(C::J, V::I);
        case U'づ': return cv(C::Z, V::U);
        case U'で': return cv(C::D, V::E);
        case U'ど': return cv(C::D, V::O);
        case U'な': return cv(C::N, V::A);
        case U'に': return cv(C::N, V::I);
        case U'ぬ': return cv(C::N, V::U);
        case U'ね': return cv(C::N, V::E);
        case U'の': return cv(C::N, V::O);
        case U'は': return cv(C::H, V::A);
        case U'ひ': return cv(C::Hy, V::I);
        case U'ふ': return cv(C::F, V::U);
        case U'へ': return cv(C::H, V::E);
        case U'ほ': return cv(C::H, V::O);
        case U'ば': return cv(C::B, V::A);
        case U'び': return cv(C::B, V::I);
        case U'ぶ': return cv(C::B, V::U);
        case U'べ': return cv(C::B, V::E);
        case U'ぼ': return cv(C::B, V::O);
        case U'ぱ': return cv(C::P, V::A);
        case U'ぴ': return cv(C::P, V::I);
        case U'ぷ': return cv(C::P, V::U);
        case U'ぺ': return cv(C::P, V::E);
        case U'ぽ': return cv(C::P, V::O);
        case U'ま': return cv(C::M, V::A);
        case U'み': return cv(C::M, V::I);
        case U'む': return cv(C::M, V::U);
        case U'め': return cv(C::M, V::E);
        case U'も': return cv(C::M, V::O);
        case U'や': return cv(C::Y, V::A);
        case U'ゆ': return cv(C::Y, V::U);
        case U'よ': return cv(C::Y, V::O);
        case U'ら': return cv(C::R, V::A);
        case U'り': return cv(C::R, V::I);
        case U'る': return cv(C::R, V::U);
        case U'れ': return cv(C::R, V::E);
        case U'ろ': return cv(C::R, V::O);
        case U'わ': return cv(C::W, V::A);
        case U'ゐ': return cv(C::W, V::I);
        case U'ゑ': return cv(C::W, V::E);
        case U'を': return cv(C::None, V::O);
        default: return false;
    }
}

}  // namespace

bool parse_kana(std::u32string_view kana, std::vector<Mora>& out) {
    out.clear();
    for (std::size_t i = 0; i < kana.size(); ++i) {
        char32_t ch = kana[i];
        if (ch == U'、' || ch == U'，' || ch == U',' || ch == U'。' || ch == U'.' || ch == U' ' || ch == U'　') {
            if (!out.empty() && out.back().kind != MoraKind::Pause) {
                out.push_back(Mora{MoraKind::Pause});
            }
            continue;
        }
        if (ch == U'\n' || ch == U'\r' || ch == U'\t') {
            continue;
        }
        if (ch == U'っ' || ch == U'ッ') {
            out.push_back(Mora{MoraKind::Sokuon});
            continue;
        }
        if (ch == U'ー') {
            out.push_back(Mora{MoraKind::Chouon});
            continue;
        }
        if (ch == U'ん' || ch == U'ン') {
            out.push_back(Mora{MoraKind::MoraicN});
            continue;
        }
        if ((ch == U'ゃ' || ch == U'ゅ' || ch == U'ょ') && !out.empty() &&
            out.back().kind == MoraKind::CV && out.back().v == Vowel::I &&
            out.back().c != Consonant::None) {
            Vowel sv = (ch == U'ゃ') ? Vowel::A : (ch == U'ゅ') ? Vowel::U : Vowel::O;
            out.back().v = sv;
            out.back().palatalized = true;
            if (out.back().c == Consonant::Sh) {
            } else if (out.back().c == Consonant::Ch || out.back().c == Consonant::J) {
            }
            continue;
        }
        Mora m;
        if (basic_mora(ch, m)) {
            out.push_back(m);
        }
    }
    return !out.empty();
}

}  // namespace stackchan::jtts::internal
