// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "avatar_vm/storage.hpp"

#include <esp_log.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>

namespace stackchan::avatar_vm::storage {

namespace {
constexpr const char* kTag = "avatar_vm";
constexpr const char* kNs = "avatar_vm";
constexpr const char* kKey = "face_bc";
// Dedicated NVS lives on the (otherwise unused) 1 MB "storage" partition.
// The main "nvs" partition is only 16 KiB and sits essentially full with the
// 35 DeviceConfig keys + Wi-Fi PHY calibration; a multi-KB bytecode blob
// update (which transiently needs old + new copies) fails there with
// ESP_ERR_NVS_NOT_ENOUGH_SPACE (observed on HW with an 1.8 KB face). The
// partition is declared subtype `spiffs` in partitions.csv but has never
// been formatted or mounted, so we claim it via nvs_flash_init_partition_ptr
// (which, unlike nvs_flash_init_partition, doesn't insist on subtype nvs —
// important because OTA updates never rewrite the partition table on
// existing devices).
constexpr const char* kPart = "storage";

// One-time init of the dedicated partition. Returns true when the "storage"
// NVS is usable; false → callers fall back to the legacy main-NVS namespace
// (defensive: keeps exotic/older partition tables working).
bool ensure_partition() noexcept
{
    static enum class State : std::uint8_t { Unknown, Ok, Absent } state = State::Unknown;
    if (state != State::Unknown) return state == State::Ok;

    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPart);
    if (part == nullptr) {
        ESP_LOGW(kTag, "no \"%s\" partition — falling back to main NVS", kPart);
        state = State::Absent;
        return false;
    }
    esp_err_t err = nvs_flash_init_partition_ptr(part);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Blank flash initialises cleanly; this branch means the region holds
        // non-NVS leftovers (or a future NVS format). It has never carried
        // user data in this firmware, so reformat and retry.
        ESP_LOGW(kTag, "\"%s\" partition unusable (%s) — erasing and retrying",
                 kPart, esp_err_to_name(err));
        if (esp_partition_erase_range(part, 0, part->size) == ESP_OK) {
            err = nvs_flash_init_partition_ptr(part);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs init on \"%s\" failed: %s — falling back to main NVS",
                 kPart, esp_err_to_name(err));
        state = State::Absent;
        return false;
    }
    state = State::Ok;
    return true;
}

// Open the bytecode namespace on whichever store is active. `partition` is
// true for the dedicated store, false for the legacy main-NVS namespace.
esp_err_t open_store(bool partition, nvs_open_mode_t mode, nvs_handle_t& h) noexcept
{
    return partition ? nvs_open_from_partition(kPart, kNs, mode, &h)
                     : nvs_open(kNs, mode, &h);
}

tl::expected<std::vector<std::uint8_t>, StorageError> load_from(bool partition) noexcept
{
    nvs_handle_t h;
    esp_err_t err = open_store(partition, NVS_READONLY, h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return tl::unexpected(StorageError::NotFound);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(RO,%s): %s", partition ? kPart : "nvs", esp_err_to_name(err));
        return tl::unexpected(StorageError::NvsOpen);
    }
    std::size_t len = 0;
    err = nvs_get_blob(h, kKey, nullptr, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || len == 0) {
        nvs_close(h);
        return tl::unexpected(StorageError::NotFound);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob(size): %s", esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(StorageError::ReadFailed);
    }
    std::vector<std::uint8_t> buf(len);
    err = nvs_get_blob(h, kKey, buf.data(), &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_blob(read): %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::ReadFailed);
    }
    return buf;
}

tl::expected<void, StorageError> save_to(bool partition,
                                         std::span<const std::uint8_t> bytes) noexcept
{
    nvs_handle_t h;
    esp_err_t err = open_store(partition, NVS_READWRITE, h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_open(RW,%s): %s", partition ? kPart : "nvs", esp_err_to_name(err));
        return tl::unexpected(StorageError::NvsOpen);
    }
    err = nvs_set_blob(h, kKey, bytes.data(), bytes.size());
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_set_blob: %s", esp_err_to_name(err));
        nvs_close(h);
        return tl::unexpected(StorageError::WriteFailed);
    }
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_commit: %s", esp_err_to_name(err));
        return tl::unexpected(StorageError::CommitFailed);
    }
    return {};
}

// Best-effort removal of the blob from the legacy main-NVS namespace. Frees
// ~1 KB of the tiny main partition (which also unblocks the PHY-calibration
// store that was failing with NOT_ENOUGH_SPACE alongside us).
void erase_legacy() noexcept
{
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_erase_key(h, kKey);
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(kTag, "legacy face_bc removed from main NVS");
    }
    nvs_close(h);
}

} // namespace

const char* to_string(StorageError e) noexcept
{
    switch (e) {
    case StorageError::NvsOpen: return "nvs_open failed";
    case StorageError::NotFound: return "not found";
    case StorageError::ReadFailed: return "nvs read failed";
    case StorageError::WriteFailed: return "nvs write failed";
    case StorageError::CommitFailed: return "nvs commit failed";
    case StorageError::EraseFailed: return "nvs erase failed";
    case StorageError::TooLarge: return "bytecode too large";
    }
    return "?";
}

tl::expected<std::vector<std::uint8_t>, StorageError> load() noexcept
{
    if (!ensure_partition()) return load_from(false);

    auto res = load_from(true);
    if (res || res.error() != StorageError::NotFound) return res;

    // Nothing in the dedicated store — migrate a pre-existing blob from the
    // legacy main-NVS namespace (installs that saved a face before the store
    // moved). The copy is the source of truth from here on.
    auto legacy = load_from(false);
    if (!legacy) return legacy;
    ESP_LOGI(kTag, "migrating %u B face bytecode from main NVS to \"%s\"",
             static_cast<unsigned>(legacy->size()), kPart);
    if (save_to(true, *legacy)) {
        erase_legacy();
    }
    return legacy;
}

tl::expected<void, StorageError> save(std::span<const std::uint8_t> bytes) noexcept
{
    if (bytes.size() > kMaxBytecodeBytes) {
        return tl::unexpected(StorageError::TooLarge);
    }
    // Validate before persisting — never store a blob the VM cannot run.
    auto bc = decode(bytes);
    if (!bc) {
        ESP_LOGW(kTag, "save: decode rejected (%s)", to_string(bc.error()));
        return tl::unexpected(StorageError::WriteFailed);
    }
    const bool partition = ensure_partition();
    auto res = save_to(partition, bytes);
    if (res && partition) {
        erase_legacy(); // stale copy must not shadow future fallback loads
    }
    return res;
}

tl::expected<void, StorageError> clear() noexcept
{
    // Erase in BOTH stores: leaving the legacy copy behind would resurrect a
    // cleared face through the migration path on the next boot.
    auto erase_in = [](bool p) -> tl::expected<void, StorageError> {
        nvs_handle_t h;
        esp_err_t err = open_store(p, NVS_READWRITE, h);
        if (err == ESP_ERR_NVS_NOT_FOUND) return {}; // namespace never created
        if (err != ESP_OK) return tl::unexpected(StorageError::NvsOpen);
        err = nvs_erase_key(h, kKey);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(h);
            return {}; // already absent
        }
        if (err != ESP_OK) {
            nvs_close(h);
            return tl::unexpected(StorageError::EraseFailed);
        }
        err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) return tl::unexpected(StorageError::CommitFailed);
        return {};
    };
    auto res = ensure_partition() ? erase_in(true) : tl::expected<void, StorageError>{};
    auto legacy = erase_in(false);
    if (!res) return res;
    return legacy;
}

} // namespace stackchan::avatar_vm::storage
