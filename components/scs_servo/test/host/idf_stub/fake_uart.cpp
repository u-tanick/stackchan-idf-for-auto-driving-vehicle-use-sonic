// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Implementation of the ESP-IDF UART driver stubs against the FakeUart wire.

#include "fake_uart.hpp"

#include <cstring>

#include "driver/uart.h"

namespace scs_hosttest {

FakeUart& fake_uart()
{
    static FakeUart inst;
    return inst;
}

} // namespace scs_hosttest

using scs_hosttest::fake_uart;

extern "C" {

esp_err_t uart_driver_install(uart_port_t, int, int, int, void*, int) { return ESP_OK; }
esp_err_t uart_driver_delete(uart_port_t) { return ESP_OK; }
esp_err_t uart_param_config(uart_port_t, const uart_config_t*) { return ESP_OK; }
esp_err_t uart_set_pin(uart_port_t, int, int, int, int) { return ESP_OK; }

esp_err_t uart_flush_input(uart_port_t)
{
    // No-op on the host. On hardware this clears the RX FIFO before a write,
    // draining stale bytes that arrived before the request. In the test model
    // the scripted RX queue *is* the servo's forthcoming response (which on
    // real hardware would only land after the write), so flushing it here
    // would wrongly discard the reply. Keeping it a no-op lets a test pre-load
    // the exact response bytes transact() should parse.
    return ESP_OK;
}

esp_err_t uart_wait_tx_done(uart_port_t, unsigned int)
{
    return fake_uart().tx_done_ok ? ESP_OK : ESP_FAIL;
}

int uart_write_bytes(uart_port_t, const void* src, size_t size)
{
    auto& u = fake_uart();
    const auto* p = static_cast<const std::uint8_t*>(src);
    u.tx.insert(u.tx.end(), p, p + size);
    if (u.forced_write_return != -2) {
        return u.forced_write_return;
    }
    return static_cast<int>(size);
}

int uart_read_bytes(uart_port_t, void* buf, uint32_t length, unsigned int)
{
    auto& u = fake_uart();
    auto* out = static_cast<std::uint8_t*>(buf);
    uint32_t n = 0;
    while (n < length && !u.rx.empty()) {
        out[n++] = u.rx.front();
        u.rx.pop_front();
    }
    return static_cast<int>(n);
}

} // extern "C"
