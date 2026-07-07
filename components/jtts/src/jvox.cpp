// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
#include "jtts/jvox.hpp"

#include <cstring>

namespace stackchan::jtts::jvox {

namespace {

constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kUnitRecSize = 16;

std::uint16_t rd_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t rd_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

struct UnitRec {
    std::uint16_t key;
    std::uint16_t mark_count;
    std::uint32_t pcm_off;
    std::uint32_t pcm_len;
    std::uint16_t steady_start;
    std::uint16_t orig_f0_dhz;
};

UnitRec read_unit(const std::uint8_t* p) {
    UnitRec r;
    r.key = rd_u16(p + 0);
    r.mark_count = rd_u16(p + 2);
    r.pcm_off = rd_u32(p + 4);
    r.pcm_len = rd_u32(p + 8);
    r.steady_start = rd_u16(p + 12);
    r.orig_f0_dhz = rd_u16(p + 14);
    return r;
}

}  // namespace

std::optional<Db> Db::parse(std::span<const std::uint8_t> blob) {
    if (blob.size() < kHeaderSize) return std::nullopt;
    if (std::memcmp(blob.data(), "JVOX", 4) != 0) return std::nullopt;
    if (blob[4] != 1) return std::nullopt;  // version
    if (blob[5] != 0) return std::nullopt;  // codec: i16 のみ
    // marks / pcm を u16/i16 として直接指すため 2 バイト アライン必須。
    if ((reinterpret_cast<std::uintptr_t>(blob.data()) & 1u) != 0) return std::nullopt;

    Db db;
    db.blob_ = blob;
    db.sample_rate_ = rd_u16(blob.data() + 6);
    db.unit_count_ = rd_u16(blob.data() + 8);
    if (db.sample_rate_ < 8000) return std::nullopt;

    std::size_t off = kHeaderSize;
    if (blob.size() < off + kUnitRecSize * db.unit_count_ + 4) return std::nullopt;
    db.units_ = blob.data() + off;
    off += kUnitRecSize * db.unit_count_;

    db.marks_total_ = rd_u32(blob.data() + off);
    off += 4;
    if (blob.size() < off + 2u * db.marks_total_) return std::nullopt;
    db.marks_ = reinterpret_cast<const std::uint16_t*>(blob.data() + off);
    off += 2u * db.marks_total_;

    if ((blob.size() - off) % 2 != 0) return std::nullopt;
    db.pcm_ = reinterpret_cast<const std::int16_t*>(blob.data() + off);
    db.pcm_total_ = static_cast<std::uint32_t>((blob.size() - off) / 2);

    // 全レコードの範囲を前もって検証しておく (find() 側を無検査にできる)。
    std::uint32_t marks_seen = 0;
    for (std::uint16_t i = 0; i < db.unit_count_; ++i) {
        UnitRec r = read_unit(db.units_ + kUnitRecSize * i);
        if (r.pcm_off + r.pcm_len > db.pcm_total_) return std::nullopt;
        if (r.pcm_len > 0xFFFF) return std::nullopt;  // marks が u16 で指せる範囲
        marks_seen += r.mark_count;
        if (marks_seen > db.marks_total_) return std::nullopt;
    }
    return db;
}

std::optional<UnitView> Db::find(std::uint16_t key) const {
    std::uint32_t mark_cursor = 0;
    for (std::uint16_t i = 0; i < unit_count_; ++i) {
        UnitRec r = read_unit(units_ + kUnitRecSize * i);
        if (r.key == key) {
            UnitView v;
            v.pcm = {pcm_ + r.pcm_off, r.pcm_len};
            v.marks = {marks_ + mark_cursor, r.mark_count};
            v.steady_start = r.steady_start;
            v.orig_f0_hz = static_cast<float>(r.orig_f0_dhz) * 0.1f;
            return v;
        }
        mark_cursor += r.mark_count;
    }
    return std::nullopt;
}

}  // namespace stackchan::jtts::jvox
