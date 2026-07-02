// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Shared helpers for the avatar_vm host tests: a minimal assert-based check
// harness (in the same spirit as jtts/test/host) and a Canvas implementation
// that records draw calls instead of touching real hardware.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "avatar/canvas.hpp"
#include "avatar/draw_context.hpp"
#include "avatar/face_tuning.hpp"
#include "avatar_vm/bytecode.hpp"

namespace avtest {

// --- tiny check harness ---------------------------------------------------
inline int g_failures = 0;
inline int g_checks = 0;

inline void report(bool ok, const char* expr, const char* file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s:%d  CHECK(%s)\n", file, line, expr);
    }
}

#define CHECK(cond) ::avtest::report((cond), #cond, __FILE__, __LINE__)

inline int finish(const char* suite)
{
    if (g_failures == 0) {
        std::printf("[ OK ] %s: %d checks passed\n", suite, g_checks);
        return 0;
    }
    std::fprintf(stderr, "%s: %d/%d checks FAILED\n", suite, g_failures, g_checks);
    return 1;
}

// --- recording canvas -----------------------------------------------------
// Implements the avatar::Canvas primitive surface. Every draw primitive is
// appended to `ops` so a test can assert what the VM emitted. Dimensions are
// configurable so the design->screen scale maths in the VM can be exercised.
struct DrawOp {
    enum class Kind { FillRect, FillCircle, FillTriangle, BeginGroup, EndGroup };
    Kind kind;
    std::int32_t a{}, b{}, c{}, d{}, e{}, f{};
    std::uint16_t color{};
};

class RecordingCanvas final : public stackchan::avatar::Canvas {
public:
    RecordingCanvas(std::int32_t w, std::int32_t h, bool circular = false)
        : w_{w}, h_{h}, circular_{circular}
    {
    }

    std::int32_t width() const override { return w_; }
    std::int32_t height() const override { return h_; }
    bool is_circular() const noexcept override { return circular_; }

    void fillScreen(std::uint16_t) override {}
    void fillRect(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::uint16_t color) override
    {
        ops.push_back({DrawOp::Kind::FillRect, x, y, w, h, 0, 0, color});
    }
    void fillCircle(std::int32_t x, std::int32_t y, std::int32_t r, std::uint16_t color) override
    {
        ops.push_back({DrawOp::Kind::FillCircle, x, y, r, 0, 0, 0, color});
    }
    void fillTriangle(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::int32_t x2, std::int32_t y2, std::uint16_t color) override
    {
        ops.push_back({DrawOp::Kind::FillTriangle, x0, y0, x1, y1, x2, y2, color});
    }
    void begin_group(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) override
    {
        ops.push_back({DrawOp::Kind::BeginGroup, x, y, w, h, 0, 0, 0});
    }
    void end_group() override { ops.push_back({DrawOp::Kind::EndGroup, 0, 0, 0, 0, 0, 0, 0}); }

    void begin_frame(std::uint16_t) override {}
    void end_frame() override {}
    void request_full_repaint() override {}

    std::vector<DrawOp> ops;

private:
    std::int32_t w_, h_;
    bool circular_;
};

// --- bytecode builder -----------------------------------------------------
// Assembles a valid .avbc buffer with a 16-byte header, a const table, a fn
// table and a code section. Mirrors the on-wire layout the decoder parses.
class BytecodeBuilder {
public:
    // Const table entries (tag + value), little-endian encoded.
    void add_const_f32(float v)
    {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        consts_.push_back(0x01);
        push_u32(consts_, u);
    }
    void add_const_i32(std::int32_t v)
    {
        consts_.push_back(0x02);
        push_u32(consts_, static_cast<std::uint32_t>(v));
    }
    void add_const_color(std::uint16_t v)
    {
        consts_.push_back(0x03);
        consts_.push_back(static_cast<std::uint8_t>(v & 0xFF));
        consts_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    }
    void add_raw_const_byte(std::uint8_t b) { consts_.push_back(b); } // for malformed cases

    // fn table entry: code_offset(2) param_count(1) local_count(1) reserved(2)
    void add_fn(std::uint16_t code_offset, std::uint8_t param_count, std::uint8_t local_count)
    {
        push_u16(fns_, code_offset);
        fns_.push_back(param_count);
        fns_.push_back(local_count);
        fns_.push_back(0);
        fns_.push_back(0);
    }

    void code(std::uint8_t b) { code_.push_back(b); }
    void code_u16(std::uint16_t v) { push_u16(code_, v); }
    void code_i16(std::int16_t v) { push_u16(code_, static_cast<std::uint16_t>(v)); }
    void code_f32(float v)
    {
        std::uint32_t u;
        std::memcpy(&u, &v, 4);
        push_u32(code_, u);
    }

    // Explicit counts let malformed tests declare a count that mismatches the
    // bytes actually appended. Defaults derive them from what was added.
    std::vector<std::uint8_t> build(std::uint16_t entry_fn = 0, int const_count = -1,
                                    int fn_count = -1, int code_size = -1) const
    {
        std::vector<std::uint8_t> out;
        push_u32(out, 0x53445641u);                 // magic "AVDS"
        push_u16(out, 1);                            // version
        push_u16(out, 0);                            // reserved
        push_u16(out, static_cast<std::uint16_t>(const_count < 0 ? auto_const_count() : const_count));
        push_u16(out, static_cast<std::uint16_t>(fn_count < 0 ? (fns_.size() / 6) : fn_count));
        push_u16(out, static_cast<std::uint16_t>(code_size < 0 ? code_.size() : code_size));
        push_u16(out, entry_fn);
        out.insert(out.end(), consts_.begin(), consts_.end());
        out.insert(out.end(), fns_.begin(), fns_.end());
        out.insert(out.end(), code_.begin(), code_.end());
        return out;
    }

    std::size_t code_len() const { return code_.size(); }

private:
    // Count const entries by walking tag bytes (only used when caller omits an
    // explicit count and the const table is well-formed).
    std::uint16_t auto_const_count() const
    {
        std::size_t i = 0, n = 0;
        while (i < consts_.size()) {
            std::uint8_t tag = consts_[i++];
            if (tag == 0x03) i += 2;
            else i += 4;
            ++n;
        }
        return static_cast<std::uint16_t>(n);
    }
    static void push_u16(std::vector<std::uint8_t>& v, std::uint16_t x)
    {
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    }
    static void push_u32(std::vector<std::uint8_t>& v, std::uint32_t x)
    {
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 16) & 0xFF));
        v.push_back(static_cast<std::uint8_t>((x >> 24) & 0xFF));
    }

    std::vector<std::uint8_t> consts_;
    std::vector<std::uint8_t> fns_;
    std::vector<std::uint8_t> code_;
};

} // namespace avtest
