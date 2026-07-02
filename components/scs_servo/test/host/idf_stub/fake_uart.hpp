// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Host-side fake UART backing the ESP-IDF driver stubs. Lets the *real*
// scs_bus.cpp / scs_servo.cpp compile and run on the host: every byte the code
// under test writes is captured in `tx`, and reads are served from a
// caller-scripted `rx` queue. Tests inspect `tx` to assert the exact wire
// bytes (framing + checksum) the production code produced, and pre-load `rx`
// to drive response-parsing paths.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace scs_hosttest {

struct FakeUart {
    std::vector<std::uint8_t> tx;       // everything written via uart_write_bytes
    std::deque<std::uint8_t> rx;        // bytes uart_read_bytes will hand back
    bool tx_done_ok = true;             // uart_wait_tx_done result
    int forced_write_return = -2;       // -2 == "return the real count"; else this value

    void reset()
    {
        tx.clear();
        rx.clear();
        tx_done_ok = true;
        forced_write_return = -2;
    }
    void push_rx(std::initializer_list<std::uint8_t> bytes)
    {
        for (auto b : bytes) rx.push_back(b);
    }
    void push_rx(const std::vector<std::uint8_t>& bytes)
    {
        for (auto b : bytes) rx.push_back(b);
    }
};

// Single global instance the driver stubs route through. UART port index is
// ignored (tests use one bus at a time).
FakeUart& fake_uart();

} // namespace scs_hosttest
