// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Header-only maths tests: raw<->degree conversion (with clamping) and the
// trapezoidal-velocity PathGenerator. These need no ESP-IDF stubs.

#include <cmath>
#include <cstdint>

#include "scs_servo/path_generator.hpp"
#include "scs_servo/scs_servo.hpp"
#include "test_support.hpp"

using namespace stackchan::scs_servo;

namespace {
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
}

int main()
{
    // --- raw_to_deg / deg_to_raw round-trip at the zero point ------------
    {
        CHECK(near(raw_to_deg(kYawZero, kYawZero), 0.0f));
        CHECK(near(raw_to_deg(kPitchZero, kPitchZero), 0.0f));
        CHECK(deg_to_raw(0.0f, kYawZero) == kYawZero);
    }

    // --- one step ≈ 0.3125° (5/16) ---------------------------------------
    {
        // 16 raw steps == 5 degrees.
        CHECK(near(raw_to_deg(kYawZero + 16, kYawZero), 5.0f));
        CHECK(near(raw_to_deg(kYawZero - 16, kYawZero), -5.0f));
        CHECK(deg_to_raw(5.0f, kYawZero) == static_cast<std::uint16_t>(kYawZero + 16));
    }

    // --- deg_to_raw clamps to the SCS0009 0..1023 range ------------------
    {
        CHECK(deg_to_raw(10000.0f, kYawZero) == 1023);
        CHECK(deg_to_raw(-10000.0f, kYawZero) == 0);
    }

    // --- PathGenerator: no move when target == current -------------------
    {
        PathGenerator<256> pg(500, /*accel=*/2.0f, /*vel=*/10.0f);
        CHECK(!pg.is_moving());
        CHECK(pg.current() == 500);
        CHECK(pg.target() == 500);
    }

    // --- PathGenerator: a forward move ends exactly on target ------------
    {
        PathGenerator<256> pg(100, 2.0f, 10.0f);
        pg.begin_move_to(400);
        CHECK(pg.is_moving());
        CHECK(pg.target() == 400);
        std::uint32_t last = pg.current();
        std::uint32_t prev = last;
        int guard = 0;
        while (pg.is_moving() && guard++ < 10000) {
            last = pg.step_next();
            // Monotonic non-decreasing toward the target for a forward move.
            CHECK(last >= prev);
            prev = last;
        }
        CHECK(!pg.is_moving());
        CHECK(pg.current() == 400);
        CHECK(last == 400);
    }

    // --- PathGenerator: a reverse move ends exactly on target ------------
    {
        PathGenerator<256> pg(800, 2.0f, 10.0f);
        pg.begin_move_to(200);
        std::uint32_t prev = pg.current();
        int guard = 0;
        while (pg.is_moving() && guard++ < 10000) {
            std::uint32_t p = pg.step_next();
            CHECK(p <= prev); // monotonic non-increasing
            prev = p;
        }
        CHECK(pg.current() == 200);
    }

    // --- PathGenerator: a very short move still converges ----------------
    {
        PathGenerator<256> pg(500, 2.0f, 10.0f);
        pg.begin_move_to(503); // distance 3, less than the accel ramp
        int guard = 0;
        while (pg.is_moving() && guard++ < 1000) pg.step_next();
        CHECK(pg.current() == 503);
    }

    // --- PathGenerator: force_stop() halts at the current sample ---------
    {
        PathGenerator<256> pg(0, 2.0f, 10.0f);
        pg.begin_move_to(1000);
        pg.step_next();
        pg.step_next();
        const std::uint32_t here = pg.current();
        pg.force_stop();
        CHECK(!pg.is_moving());
        CHECK(pg.target() == here);
    }

    // --- PathGenerator: waypoint count is capped at MaxWaypoints ---------
    // A tiny buffer plus a long, slow move would overrun waypoints_ if the
    // step count weren't clamped; stepping past the cap must still terminate
    // on target (step_next snaps to target once the waypoints run out).
    {
        PathGenerator<8> pg(0, 0.01f, 0.1f); // slow → many nominal steps
        pg.begin_move_to(1000);
        int guard = 0;
        while (pg.is_moving() && guard++ < 100) pg.step_next();
        CHECK(!pg.is_moving());
        CHECK(pg.current() == 1000);
    }

    return scstest::finish("math");
}
