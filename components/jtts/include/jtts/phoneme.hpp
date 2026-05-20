// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

namespace stackchan::jtts {

enum class Vowel : std::uint8_t { None, A, I, U, E, O };

enum class Consonant : std::uint8_t {
    None,
    K, G,
    S, Z,
    Sh, J,
    T, D,
    Ts,
    Ch,
    N,
    H,
    F,
    Hy,
    B, P,
    M,
    Y,
    R,
    W,
};

constexpr bool is_voiced_stop(Consonant c) {
    return c == Consonant::G || c == Consonant::D || c == Consonant::B;
}
constexpr bool is_voiceless_stop(Consonant c) {
    return c == Consonant::K || c == Consonant::T || c == Consonant::P;
}
constexpr bool is_voiced_fric_affric(Consonant c) {
    return c == Consonant::Z || c == Consonant::J;
}
constexpr bool is_voiceless_fric(Consonant c) {
    return c == Consonant::S || c == Consonant::Sh || c == Consonant::H || c == Consonant::F ||
           c == Consonant::Hy;
}
constexpr bool is_affricate(Consonant c) {
    return c == Consonant::Ts || c == Consonant::Ch;
}
constexpr bool is_nasal(Consonant c) {
    return c == Consonant::M || c == Consonant::N;
}
constexpr bool is_glide(Consonant c) {
    return c == Consonant::Y || c == Consonant::W;
}

constexpr bool is_voiceless_consonant(Consonant c) {
    return is_voiceless_stop(c) || is_voiceless_fric(c) || is_affricate(c);
}

enum class MoraKind : std::uint8_t {
    CV,
    MoraicN,
    Sokuon,
    Chouon,
};

struct Mora {
    MoraKind kind = MoraKind::CV;
    Consonant c = Consonant::None;
    Vowel v = Vowel::None;
    bool palatalized = false;
    // 東京式の無声化: /i/ /u/ が無声子音の間 (もしくは無声子音+文末) で
    // 声帯振動を止める。apply_devoicing() で立つ。
    bool devoiced = false;
};

}  // namespace stackchan::jtts
