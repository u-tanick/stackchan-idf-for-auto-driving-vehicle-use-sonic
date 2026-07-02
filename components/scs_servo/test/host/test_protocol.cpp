// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Protocol tests for the real ScsBus / ScsServo sources, run on the host
// against a fake UART wire (idf_stub/). Covers:
//   * TX packet framing + checksum (send / transact build the right bytes)
//   * higher-level ScsServo commands (ping / write_goal_position / read pos)
//   * response parsing: happy path, bad header, id mismatch, bad length,
//     checksum mismatch, servo-error flag, short read (timeout), oversized
//     data (buffer too small), and over-long params rejected up front.

#include <cstdint>
#include <span>
#include <vector>

#include "fake_uart.hpp"
#include "scs_servo/scs_bus.hpp"
#include "scs_servo/scs_servo.hpp"
#include "test_support.hpp"

using namespace stackchan::scs_servo;
using scs_hosttest::fake_uart;

namespace {

// Reference checksum matching the servo protocol: ~(sum of bytes) truncated to
// 8 bits, computed over [ID, Length, ...body].
std::uint8_t checksum(std::span<const std::uint8_t> bytes)
{
    std::uint32_t sum = 0;
    for (auto b : bytes) sum += b;
    return static_cast<std::uint8_t>(~sum);
}

// Build a well-formed status response packet:
//   FF FF | ID | LEN | ERR | data... | CHK
// where LEN = data.size() + 2 (err + checksum), CHK over [ID, LEN, ERR, data].
std::vector<std::uint8_t> make_response(std::uint8_t id, std::uint8_t err,
                                        std::initializer_list<std::uint8_t> data)
{
    std::vector<std::uint8_t> body;
    body.push_back(id);
    body.push_back(static_cast<std::uint8_t>(data.size() + 2));
    body.push_back(err);
    for (auto b : data) body.push_back(b);
    std::uint8_t chk = checksum(std::span<const std::uint8_t>(body));

    std::vector<std::uint8_t> pkt{0xFF, 0xFF};
    pkt.insert(pkt.end(), body.begin(), body.end());
    pkt.push_back(chk);
    return pkt;
}

ScsBus make_bus()
{
    ScsBus::Config cfg;
    cfg.uart = 1;
    cfg.tx = 6;
    cfg.rx = 7;
    auto r = ScsBus::create(cfg);
    return std::move(*r); // create() never fails against the stub driver
}

} // namespace

int main()
{
    // --- TX framing + checksum for a send() with params ------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        const std::array<std::uint8_t, 2> params{0x28, 0x01};
        auto r = bus.send(/*id=*/1, /*instruction=*/0x03, params);
        CHECK(r.has_value());
        const auto& tx = fake_uart().tx;
        // Expected: FF FF 01 04 03 28 01 CHK ; LEN = params(2)+2 = 4.
        CHECK(tx.size() == 8);
        if (tx.size() == 8) {
            CHECK(tx[0] == 0xFF && tx[1] == 0xFF);
            CHECK(tx[2] == 0x01); // id
            CHECK(tx[3] == 0x04); // length
            CHECK(tx[4] == 0x03); // instruction
            CHECK(tx[5] == 0x28 && tx[6] == 0x01);
            std::array<std::uint8_t, 5> body{0x01, 0x04, 0x03, 0x28, 0x01};
            CHECK(tx[7] == checksum(std::span<const std::uint8_t>(body)));
        }
    }

    // --- send() rejects over-long params ---------------------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        std::vector<std::uint8_t> big(33, 0); // kMaxParams == 32
        auto r = bus.send(1, 0x03, big);
        CHECK(!r && r.error() == ScsError::BufferTooSmall);
        CHECK(fake_uart().tx.empty()); // nothing written
    }

    // --- send() surfaces a tx-done timeout -------------------------------
    {
        fake_uart().reset();
        fake_uart().tx_done_ok = false;
        ScsBus bus = make_bus();
        auto r = bus.send(1, 0x01, {});
        CHECK(!r && r.error() == ScsError::Timeout);
    }

    // --- transact() happy path parses the data field ---------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx(make_response(/*id=*/1, /*err=*/0, {0xAB, 0xCD}));
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x02, std::array<std::uint8_t, 2>{0x38, 2}, scratch);
        CHECK(r.has_value());
        if (r) {
            CHECK(r->size() == 2);
            CHECK((*r)[0] == 0xAB && (*r)[1] == 0xCD);
        }
    }

    // --- transact() bad header (first sync byte wrong) -------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx({0x00, 0xFF, 0x01, 0x02, 0x00, 0xFC});
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x01, {}, scratch);
        CHECK(!r && r.error() == ScsError::BadHeader);
    }

    // --- transact() id mismatch ------------------------------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx(make_response(/*id=*/2, 0, {})); // asked for id 1
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x01, {}, scratch);
        CHECK(!r && r.error() == ScsError::IdMismatch);
    }

    // --- transact() bad length (length < 2) ------------------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx({0xFF, 0xFF, 0x01, 0x01, 0x00}); // length=1
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x01, {}, scratch);
        CHECK(!r && r.error() == ScsError::BadLength);
    }

    // --- transact() checksum mismatch ------------------------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        auto pkt = make_response(1, 0, {0x11, 0x22});
        pkt.back() ^= 0xFF; // corrupt the checksum byte
        fake_uart().push_rx(pkt);
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x02, {}, scratch);
        CHECK(!r && r.error() == ScsError::ChecksumMismatch);
    }

    // --- transact() servo-error flag set ---------------------------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx(make_response(1, /*err=*/0x01, {})); // overload bit
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x01, {}, scratch);
        CHECK(!r && r.error() == ScsError::ServoError);
    }

    // --- transact() short read (fewer bytes than header wants) → timeout -
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx({0xFF, 0xFF}); // header truncated
        std::array<std::uint8_t, 8> scratch{};
        auto r = bus.transact(1, 0x01, {}, scratch);
        CHECK(!r && r.error() == ScsError::Timeout);
    }

    // --- transact() data larger than scratch → BufferTooSmall ------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        fake_uart().push_rx(make_response(1, 0, {0x01, 0x02, 0x03, 0x04})); // 4 data bytes
        std::array<std::uint8_t, 2> scratch{}; // only room for 2
        auto r = bus.transact(1, 0x02, {}, scratch);
        CHECK(!r && r.error() == ScsError::BufferTooSmall);
    }

    // ============ ScsServo high-level commands ===========================

    // --- ping() sends the Ping instruction with no params ----------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        ScsServo servo{bus, 1};
        fake_uart().push_rx(make_response(1, 0, {}));
        auto r = servo.ping();
        CHECK(r.has_value());
        const auto& tx = fake_uart().tx;
        // FF FF 01 02 01 CHK
        CHECK(tx.size() == 6);
        if (tx.size() == 6) {
            CHECK(tx[2] == 0x01 && tx[3] == 0x02 && tx[4] == 0x01);
        }
    }

    // --- write_goal_position() emits big-endian register writes ----------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        ScsServo servo{bus, 1};
        fake_uart().push_rx(make_response(1, 0, {}));
        auto r = servo.write_goal_position(/*raw=*/0x0123, /*time_ms=*/0x0045, /*speed=*/0x0067);
        CHECK(r.has_value());
        const auto& tx = fake_uart().tx;
        // instruction=Write(3), reg=0x2A, then 3 big-endian u16.
        // FF FF 01 LEN 03 2A 01 23 00 45 00 67 CHK ; params = 7 → LEN = 9.
        CHECK(tx.size() == 13);
        if (tx.size() == 13) {
            CHECK(tx[3] == 0x09);
            CHECK(tx[4] == 0x03);
            CHECK(tx[5] == 0x2A);
            CHECK(tx[6] == 0x01 && tx[7] == 0x23); // raw hi/lo
            CHECK(tx[8] == 0x00 && tx[9] == 0x45); // time hi/lo
            CHECK(tx[10] == 0x00 && tx[11] == 0x67); // speed hi/lo
        }
    }

    // --- read_present_position() decodes big-endian response -------------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        ScsServo servo{bus, 1};
        fake_uart().push_rx(make_response(1, 0, {0x02, 0x01})); // 0x0201 = 513
        auto r = servo.read_present_position();
        CHECK(r.has_value());
        CHECK(r && *r == 0x0201);
    }

    // --- read_present_position() rejects a too-short data field ----------
    {
        fake_uart().reset();
        ScsBus bus = make_bus();
        ScsServo servo{bus, 1};
        fake_uart().push_rx(make_response(1, 0, {0x05})); // only 1 byte
        auto r = servo.read_present_position();
        CHECK(!r && r.error() == ScsError::BadLength);
    }

    return scstest::finish("protocol");
}
