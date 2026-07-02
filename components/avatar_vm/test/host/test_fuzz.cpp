// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Deterministic fuzz harness for the avatar_vm. Bytecode arrives over BLE / is
// loaded from NVS, so the decode + run path must survive arbitrary bytes: it
// may reject them, but must never crash, read out of bounds, or loop forever.
// We drive a fixed set of pseudo-random buffers (fixed seeds → reproducible)
// through decode(), and any that decode we also run(). Success == the process
// returns normally (ASan/UBSan in CI would trap any memory error).

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <span>
#include <unistd.h>
#include <vector>

#include "avatar_vm/bytecode.hpp"
#include "avatar_vm/vm.hpp"
#include "test_support.hpp"

using namespace stackchan::avatar_vm;

namespace {

// The VM has no per-run step budget, so a *decoded* program containing a
// backward self-jump (e.g. `Jmp` onto itself) executes a legal-but-infinite
// loop. That is a property of arbitrary bytecode, not a memory-safety bug, so
// the fuzzer must not hang on it. A wall-clock watchdog aborts the whole
// process if any single run wedges — turning a hang into a visible CI failure
// rather than a silent timeout. Well-behaved runs finish in microseconds.
[[noreturn]] void on_watchdog(int)
{
    static const char msg[] = "[FAIL] fuzz: run() exceeded watchdog (infinite loop in bytecode)\n";
    ssize_t n = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)n;
    _Exit(1);
}

void arm_watchdog()
{
    struct sigaction sa{};
    sa.sa_handler = on_watchdog;
    sigaction(SIGALRM, &sa, nullptr);
}

void run_if_decoded(const std::vector<std::uint8_t>& buf)
{
    auto dec = decode(std::span<const std::uint8_t>(buf));
    if (!dec) {
        return; // rejected cleanly — fine.
    }
    avtest::RecordingCanvas canvas{320, 240};
    stackchan::avatar::DrawContext ctx;
    stackchan::avatar::FaceTuning tuning;
    Vm vm;
    // 2-second guard per run: a bounded, correct program finishes near-
    // instantly, so any trip means an infinite-loop input rather than a slow
    // one. alarm() has 1s granularity; 2s avoids false positives under load.
    ::alarm(2);
    // We don't care whether it succeeds or errors, only that it terminates
    // without UB. The VM has no unbounded loop other than jumps, which are
    // bounds-checked; a decoded buffer's code section is finite and each
    // instruction advances pc or errors, but backward jumps could spin — the
    // real firmware relies on the compiler emitting terminating programs. For
    // the fuzzer we cap iterations by running against a tiny code section only
    // via a step budget: the VM itself has no budget, so we keep code sizes
    // small (see caller) to bound worst-case backward-jump loops.
    (void)vm.run(*dec, canvas, ctx, tuning);
    ::alarm(0); // disarm — this run terminated on its own.
}

// Build a buffer with a valid header + fn table but random code, so the
// fuzzer reaches the interpreter loop rather than bouncing off the decoder.
std::vector<std::uint8_t> valid_shell_with_random_code(std::mt19937& rng, std::size_t code_len)
{
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::uint8_t> code(code_len);
    for (auto& b : code) b = static_cast<std::uint8_t>(byte(rng));

    std::vector<std::uint8_t> out;
    auto push_u16 = [&](std::uint16_t x) {
        out.push_back(static_cast<std::uint8_t>(x & 0xFF));
        out.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
    };
    // header
    out.push_back(0x41);
    out.push_back(0x56);
    out.push_back(0x44);
    out.push_back(0x53); // magic "AVDS"
    push_u16(1);         // version
    push_u16(0);         // reserved
    push_u16(0);         // const_count
    push_u16(1);         // fn_count
    push_u16(static_cast<std::uint16_t>(code_len));
    push_u16(0); // entry_fn
    // fn 0: offset 0, 0 params, up to 8 locals so PushLocal has room.
    push_u16(0);
    out.push_back(0);
    out.push_back(8);
    out.push_back(0);
    out.push_back(0);
    out.insert(out.end(), code.begin(), code.end());
    return out;
}

} // namespace

int main()
{
    arm_watchdog();

    // 1. Pure random blobs of assorted lengths through the full decode path.
    {
        std::mt19937 rng(0xDEADBEEF);
        std::uniform_int_distribution<int> byte(0, 255);
        for (int iter = 0; iter < 20000; ++iter) {
            std::size_t len = static_cast<std::size_t>(rng() % 64);
            std::vector<std::uint8_t> buf(len);
            for (auto& b : buf) b = static_cast<std::uint8_t>(byte(rng));
            run_if_decoded(buf);
        }
        CHECK(true); // reached here without crashing
    }

    // 2. Valid header/fn-table shell with random code bodies — exercises the
    //    interpreter's operand-bounds checks directly. Small code sections keep
    //    any backward-jump loop bounded.
    {
        std::mt19937 rng(0x1234ABCD);
        for (int iter = 0; iter < 20000; ++iter) {
            std::size_t code_len = 1 + static_cast<std::size_t>(rng() % 24);
            auto buf = valid_shell_with_random_code(rng, code_len);
            run_if_decoded(buf);
        }
        CHECK(true);
    }

    // 3. Bit-flip mutations of a known-good program.
    {
        // known-good: PushI8 5, Pop, Ret
        std::vector<std::uint8_t> good;
        auto push_u16 = [&](std::uint16_t x) {
            good.push_back(static_cast<std::uint8_t>(x & 0xFF));
            good.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
        };
        good = {0x41, 0x56, 0x44, 0x53};
        push_u16(1);
        push_u16(0);
        push_u16(0);
        push_u16(1);
        push_u16(4); // code_len
        push_u16(0);
        push_u16(0);
        good.push_back(0);
        good.push_back(0);
        good.push_back(0);
        good.push_back(0);
        good.push_back(0x02); // PushI8
        good.push_back(5);
        good.push_back(0x08); // Pop
        good.push_back(0x34); // Ret

        std::mt19937 rng(0xFACEFEED);
        for (int iter = 0; iter < 20000; ++iter) {
            auto buf = good;
            // flip 1-3 random bits
            int flips = 1 + static_cast<int>(rng() % 3);
            for (int f = 0; f < flips; ++f) {
                std::size_t idx = rng() % buf.size();
                buf[idx] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            }
            run_if_decoded(buf);
        }
        CHECK(true);
    }

    return avtest::finish("fuzz");
}
