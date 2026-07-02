// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Minimal assert-based check harness shared by the scs_servo host tests
// (same spirit as jtts/test/host).

#pragma once

#include <cstdio>

namespace scstest {

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

#define CHECK(cond) ::scstest::report((cond), #cond, __FILE__, __LINE__)

inline int finish(const char* suite)
{
    if (g_failures == 0) {
        std::printf("[ OK ] %s: %d checks passed\n", suite, g_checks);
        return 0;
    }
    std::fprintf(stderr, "%s: %d/%d checks FAILED\n", suite, g_failures, g_checks);
    return 1;
}

} // namespace scstest
