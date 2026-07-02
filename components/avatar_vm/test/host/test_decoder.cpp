// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Boundary / robustness tests for avatar_vm::decode(). The decoder is the
// first line of defence against malformed bytecode delivered over BLE / NVS,
// so it must reject bad input with the right VmError rather than reading out
// of bounds.

#include <cstdint>
#include <span>
#include <vector>

#include "avatar_vm/bytecode.hpp"
#include "test_support.hpp"

using namespace stackchan::avatar_vm;
using avtest::BytecodeBuilder;

namespace {

// A minimal well-formed program: one 0-param entry fn whose code is [Ret].
std::vector<std::uint8_t> minimal_valid()
{
    BytecodeBuilder b;
    b.code(0x34); // Ret
    b.add_fn(/*code_offset=*/0, /*param_count=*/0, /*local_count=*/0);
    return b.build(/*entry_fn=*/0);
}

} // namespace

int main()
{
    // --- happy path: minimal valid program decodes -----------------------
    {
        auto buf = minimal_valid();
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->fns.size() == 1);
            CHECK(r->consts.empty());
            CHECK(r->code.size() == 1);
            CHECK(r->entry_fn_id == 0);
        }
    }

    // --- empty input -----------------------------------------------------
    {
        std::vector<std::uint8_t> empty;
        auto r = decode(std::span<const std::uint8_t>(empty));
        CHECK(!r.has_value());
        CHECK(!r && r.error() == VmError::Truncated);
    }

    // --- header shorter than 16 bytes ------------------------------------
    {
        auto buf = minimal_valid();
        for (std::size_t n = 0; n < 16; ++n) {
            std::vector<std::uint8_t> trunc(buf.begin(), buf.begin() + n);
            auto r = decode(std::span<const std::uint8_t>(trunc));
            CHECK(!r && r.error() == VmError::Truncated);
        }
    }

    // --- bad magic -------------------------------------------------------
    {
        auto buf = minimal_valid();
        buf[0] ^= 0xFF;
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::BadMagic);
    }

    // --- bad version -----------------------------------------------------
    {
        auto buf = minimal_valid();
        buf[4] = 0x99; // version low byte
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::BadVersion);
    }

    // --- const table declares more entries than bytes present ------------
    // The decoder walks tag bytes from the const region; over-declaring far
    // more consts than there are bytes must be rejected. Depending on which
    // trailing bytes it lands on it reports either Truncated (ran off the end)
    // or BadConstTag (a stray byte wasn't a valid tag) — both are correct
    // rejections; what matters is it never accepts nor reads out of bounds.
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(0, /*const_count=*/100); // vastly more than present
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && (r.error() == VmError::Truncated || r.error() == VmError::BadConstTag));
    }

    // --- unknown const tag -----------------------------------------------
    {
        BytecodeBuilder b;
        b.add_raw_const_byte(0x7F); // invalid tag
        b.add_raw_const_byte(0x00);
        b.add_raw_const_byte(0x00);
        b.add_raw_const_byte(0x00);
        b.add_raw_const_byte(0x00);
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(0, /*const_count=*/1);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::BadConstTag);
    }

    // --- const F32 tag truncated (tag present, value missing) ------------
    {
        BytecodeBuilder b;
        b.add_raw_const_byte(0x01); // F32 tag, but no 4 value bytes
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(0, /*const_count=*/1);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::Truncated);
    }

    // --- valid consts of every tag decode + promote to float -------------
    {
        BytecodeBuilder b;
        b.add_const_f32(3.5f);
        b.add_const_i32(-7);
        b.add_const_color(0xF800);
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(0);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(r.has_value());
        if (r) {
            CHECK(r->consts.size() == 3);
            CHECK(r->consts[0] == 3.5f);
            CHECK(r->consts[1] == -7.0f);
            CHECK(r->consts[2] == static_cast<float>(0xF800));
        }
    }

    // --- fn table truncated ----------------------------------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        auto buf = b.build(0, /*const_count=*/0, /*fn_count=*/2); // declares 2, has 0
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::Truncated);
    }

    // --- fn with local_count < param_count is rejected -------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(/*code_offset=*/0, /*param_count=*/3, /*local_count=*/1);
        auto buf = b.build(0);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::BadLocalSlot);
    }

    // --- code section declared longer than buffer ------------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(0, -1, -1, /*code_size=*/64); // only 1 byte present
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::Truncated);
    }

    // --- entry fn id out of range ----------------------------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(0, 0, 0);
        auto buf = b.build(/*entry_fn=*/5); // only fn 0 exists
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::EntryFnInvalid);
    }

    // --- entry fn code_offset points past code section -------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(/*code_offset=*/99, 0, 0);
        auto buf = b.build(0);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::EntryFnInvalid);
    }

    // --- entry fn with non-zero param_count is invalid -------------------
    {
        BytecodeBuilder b;
        b.code(0x34);
        b.add_fn(/*code_offset=*/0, /*param_count=*/2, /*local_count=*/2);
        auto buf = b.build(0);
        auto r = decode(std::span<const std::uint8_t>(buf));
        CHECK(!r && r.error() == VmError::EntryFnInvalid);
    }

    // --- to_string covers every enum value without crashing --------------
    {
        for (int i = 1; i <= static_cast<int>(VmError::EntryFnInvalid); ++i) {
            const char* s = to_string(static_cast<VmError>(i));
            CHECK(s != nullptr && s[0] != '\0');
        }
    }

    return avtest::finish("decoder");
}
