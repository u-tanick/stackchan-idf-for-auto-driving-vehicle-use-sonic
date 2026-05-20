// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "jtts/jtts.hpp"

#include <vector>

#include "internal.hpp"

namespace stackchan::jtts {

const char* to_string(Error e) {
    switch (e) {
        case Error::InvalidKana: return "InvalidKana";
        case Error::OutOfMemory: return "OutOfMemory";
    }
    return "Unknown";
}

namespace {

Options resolve_defaults(Options opt) {
    if (opt.f0_hz <= 0.0f) {
        opt.f0_hz = (opt.voice == Voice::Female) ? 210.0f : 130.0f;
    }
    if (opt.formant_scale <= 0.0f) {
        opt.formant_scale = (opt.voice == Voice::Female) ? 1.17f : 1.0f;
    }
    return opt;
}

void apply_formant_scale(std::vector<internal::Segment>& segs, float scale) {
    if (scale == 1.0f) return;
    auto scale_frame = [scale](internal::FormantFrame& f) {
        f.f1 *= scale;
        f.f2 *= scale;
        f.f3 *= scale;
        f.bw1 *= scale;
        f.bw2 *= scale;
        f.bw3 *= scale;
    };
    for (auto& s : segs) {
        scale_frame(s.start);
        scale_frame(s.end);
    }
}

}  // namespace

tl::expected<void, Error> synthesize(std::u32string_view kana,
                                     std::vector<std::int16_t>& out, const Options& opt_in) {
    out.clear();
    Options opt = resolve_defaults(opt_in);

    std::vector<Mora> moras;
    if (!internal::parse_kana(kana, moras)) {
        return tl::make_unexpected(Error::InvalidKana);
    }
    internal::apply_devoicing(moras);

    std::vector<internal::Segment> segs;
    internal::build_segments(moras, segs, opt);
    if (segs.empty()) {
        return tl::make_unexpected(Error::InvalidKana);
    }
    apply_formant_scale(segs, opt.formant_scale);
    internal::apply_prosody(segs, opt);

    std::size_t estimated_samples = 0;
    for (const auto& s : segs) {
        estimated_samples += static_cast<std::size_t>(s.duration_ms * 0.001f * opt.sample_rate_hz) + 16;
    }
    out.reserve(estimated_samples);

    internal::render_segments(segs, out, opt);
    return {};
}

}  // namespace stackchan::jtts
