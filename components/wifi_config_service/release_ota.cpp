// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "release_ota.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>


#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <config_service/ota.hpp>

namespace stackchan::wifi_config::release_ota {

namespace {

constexpr const char* kTag = "release-ota";

// Chunk we read from the HTTPS body and forward into esp_ota_write. 4 KiB
// matches the bound used by handle_data_chunk on the BLE / browser-OTA
// path. Bigger doesn't help — esp_ota_write internally page-aligns to
// 4 KiB anyway and we don't want to inflate the worker stack with a
// stack-resident chunk buffer.
constexpr std::size_t kReadChunk = 4096;

// Worker stack. HTTPS = mbedTLS handshake (~10 KiB peak) + esp_http_client
// + esp_ota_write (cache-disable while writing). Lives in internal RAM
// because esp_ota_write disables the CPU instruction cache around its
// flash writes, and PSRAM-resident stacks become unreadable during that
// window (kernel assert).
constexpr std::size_t kWorkerStack = 16 * 1024;

std::atomic<bool> g_active{false};
std::atomic<bool> g_abort{false};

struct WorkerArgs {
    std::string tag;
    std::uint8_t board_kind;
};

// Streaming mode (esp_http_client_open + fetch_headers + read) does NOT honour
// the client's auto-redirect: a 30x is handed back to us verbatim. GitHub Pages
// with a custom domain (www.fugafuga.org) answers the ciniml.github.io path with
// a 301, so we have to walk the Location chain by hand.
//
// After a 30x, esp_http_client_set_redirection() copies the server's Location
// into the client URL (reparsing the scheme). We then re-open + re-fetch. We cap
// the hops and UPGRADE any http:// Location to https:// before reconnecting —
// GitHub Pages' custom-domain (www.fugafuga.org) 301 hands back a literal
// plaintext "http://..." URL, but the 301 itself arrived over GitHub's valid
// TLS so the Location can't have been tampered with. Reconnecting to the same
// host/path under https means plaintext never touches the wire (an HSTS-style
// upgrade); if that host doesn't actually speak https the TLS handshake simply
// fails. get_transport_type() is unreliable right after close() (it returns a
// stale value), so the https guarantee is enforced by rewriting the URL string.
//
// Returns the final (non-redirect) HTTP status, or std::nullopt if opening /
// fetching failed, the hop cap was exceeded, or a redirect pointed at a URL
// with neither an http:// nor https:// scheme.
constexpr int kMaxRedirects = 3;

std::optional<int> open_follow_redirects(esp_http_client_handle_t client,
                                         int64_t& content_length_out)
{
    for (int hop = 0; hop <= kMaxRedirects; ++hop) {
        if (esp_err_t e = esp_http_client_open(client, 0); e != ESP_OK) {
            ESP_LOGE(kTag, "http_client_open: %s", esp_err_to_name(e));
            return std::nullopt;
        }
        content_length_out = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);

        const bool is_redirect =
            status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308;
        if (!is_redirect) {
            return status;
        }

        if (hop == kMaxRedirects) {
            ESP_LOGE(kTag, "too many redirects (>%d), last status %d",
                     kMaxRedirects, status);
            return std::nullopt;
        }

        // Point the client at Location. Must close the current connection
        // first — set_redirection only rewrites the URL, the next open()
        // establishes the new connection.
        esp_http_client_close(client);
        if (esp_err_t e = esp_http_client_set_redirection(client); e != ESP_OK) {
            ESP_LOGE(kTag, "set_redirection (status %d): %s", status,
                     esp_err_to_name(e));
            return std::nullopt;
        }
        // Force the redirect target onto https before we reconnect: the
        // Location arrived over trusted TLS, so upgrading http:// -> https://
        // keeps the whole chain encrypted (plaintext never touches the wire).
        //
        // Subtlety confirmed against IDF 5.5.4 esp_http_client.c: after
        // set_redirection() parses a plaintext "http://host/path" Location,
        // connection_info.port is pinned to 80, and esp_http_client_get_url()
        // ALWAYS re-emits the port explicitly ("%s://%s:%d%s") — i.e. we get
        // back "http://host:80/path". A naive "http://"->"https://" splice
        // would leave the stale ":80", and esp_http_client_set_url()'s UF_PORT
        // branch (which runs AFTER the scheme->default-port branch) would then
        // override the freshly-defaulted 443 back to 80, so the TLS handshake
        // lands on port 80 and fails with ESP_ERR_HTTP_CONNECT. Fix: rebuild
        // the URL with an https-appropriate port. Drop the port when it's the
        // http default (80) so https defaults to 443; keep any genuinely
        // explicit non-default port (a Location that named its own port).
        char url[256];
        if (esp_err_t e = esp_http_client_get_url(client, url, sizeof(url)); e != ESP_OK) {
            ESP_LOGE(kTag, "get_url after redirect (status %d): %s", status,
                     esp_err_to_name(e));
            return std::nullopt;
        }
        if (std::strncmp(url, "http://", 7) == 0) {
            // Split "http://<hostport>/<rest...>" at the first '/' after the
            // scheme. <hostport> is "host" or "host:port"; <rest> (with its
            // leading '/') is the path+query, or empty.
            const char* host_start = url + 7;
            const char* path_start = std::strchr(host_start, '/');
            const std::size_t host_len =
                path_start ? static_cast<std::size_t>(path_start - host_start)
                           : std::strlen(host_start);
            const char* rest = path_start ? path_start : "";

            // Separate host from an optional explicit port.
            std::string_view hostport(host_start, host_len);
            const std::size_t colon = hostport.rfind(':');
            std::string_view host = hostport;
            std::string_view port; // empty => no explicit port
            if (colon != std::string_view::npos) {
                host = hostport.substr(0, colon);
                port = hostport.substr(colon + 1);
            }

            // 80 is the http default that get_url synthesised — drop it so
            // https resolves to 443. Anything else was a real explicit port
            // and must be preserved.
            const bool port_is_http_default = (port == "80");

            char upgraded[256 + 8];
            int written;
            if (port.empty() || port_is_http_default) {
                // No explicit port (or the synthetic http default): pin :443
                // so set_url's UF_PORT branch can't fall back to an http port.
                written = std::snprintf(upgraded, sizeof(upgraded),
                                        "https://%.*s:443%s",
                                        static_cast<int>(host.size()), host.data(), rest);
            } else {
                // Honour the Location's explicit non-default port under https.
                written = std::snprintf(upgraded, sizeof(upgraded),
                                        "https://%.*s:%.*s%s",
                                        static_cast<int>(host.size()), host.data(),
                                        static_cast<int>(port.size()), port.data(), rest);
            }
            if (written < 0 || static_cast<std::size_t>(written) >= sizeof(upgraded)) {
                ESP_LOGE(kTag, "redirect URL too long to upgrade (status %d)", status);
                return std::nullopt;
            }
            ESP_LOGI(kTag, "upgrading redirect to https (status %d): %s", status, upgraded);
            if (esp_err_t e = esp_http_client_set_url(client, upgraded); e != ESP_OK) {
                ESP_LOGE(kTag, "set_url after upgrade (status %d): %s", status,
                         esp_err_to_name(e));
                return std::nullopt;
            }
        } else if (std::strncmp(url, "https://", 8) != 0) {
            ESP_LOGE(kTag, "refusing redirect with non-http(s) scheme (status %d)", status);
            return std::nullopt;
        }
        ESP_LOGI(kTag, "following redirect %d (status %d)", hop + 1, status);
    }
    return std::nullopt;
}

bool tag_looks_safe(const std::string& tag)
{
    // Tags are "vX.Y.Z" / "vX.Y.Z-rcN" in this project — alphanumerics plus
    // '.' '-' '_'. Reject anything else so a malicious / typo tag can't
    // smuggle path traversal or query strings into the URL we paste into
    // the http client.
    if (tag.empty() || tag.size() > 32) return false;
    for (char c : tag) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

void worker(void* arg)
{
    std::unique_ptr<WorkerArgs> a(static_cast<WorkerArgs*>(arg));
    g_abort.store(false, std::memory_order_release);

    const char* slug = board_slug(a->board_kind);
    if (slug == nullptr) {
        ESP_LOGE(kTag, "unknown board kind %u", static_cast<unsigned>(a->board_kind));
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    char url[256];
    // The pages.yml deploy step extracts stackchan_idf.bin out of the
    // release ZIP and stages it at this stable URL alongside the ZIP. Raw
    // (uncompressed) so the device doesn't need DEFLATE on the OTA path.
    std::snprintf(url, sizeof(url),
                  "https://ciniml.github.io/stackchan-idf/firmware/%s/%s/stackchan_idf.bin",
                  a->tag.c_str(), slug);
    ESP_LOGI(kTag, "GET %s", url);

    esp_http_client_config_t cfg{};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.keep_alive_enable = false;
    // Pages serves us a redirect for the custom-domain path; follow it.
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(kTag, "esp_http_client_init failed");
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    auto cleanup_client = [&](){
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    int64_t cl = 0;
    const std::optional<int> status_opt = open_follow_redirects(client, cl);
    if (!status_opt.has_value()) {
        // open_follow_redirects already logged the concrete failure.
        cleanup_client();
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }
    const int status = *status_opt;
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status %d (expected 200)", status);
        cleanup_client();
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }
    if (cl <= 0) {
        ESP_LOGE(kTag, "no Content-Length on bin URL");
        cleanup_client();
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }
    ESP_LOGI(kTag, "downloading %lld bytes", static_cast<long long>(cl));

    // Begin OTA via the existing dispatcher so /api/ota/status reflects
    // "receiving / N / total" the same way browser-uploaded OTA does.
    char begin_json[64];
    std::snprintf(begin_json, sizeof(begin_json),
                  R"({"op":"begin","size":%lld})", static_cast<long long>(cl));
    std::string r = config::ota::handle_control_command(begin_json);
    if (r.find("\"ok\":true") == std::string::npos) {
        ESP_LOGE(kTag, "ota begin rejected: %s", r.c_str());
        cleanup_client();
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    // Stream the body into esp_ota_write. The read chunk is heap-allocated
    // so we don't blow the 16 KiB stack — keeping a 4 KiB array as a local
    // would force the compiler to reserve it on the worker stack even
    // before the request opens.
    auto buf = std::make_unique<std::uint8_t[]>(kReadChunk);
    std::size_t total_written = 0;
    bool ok = true;
    while (total_written < static_cast<std::size_t>(cl)) {
        if (g_abort.load(std::memory_order_acquire)) {
            ESP_LOGW(kTag, "abort requested at %u / %lld bytes",
                     static_cast<unsigned>(total_written),
                     static_cast<long long>(cl));
            ok = false;
            break;
        }
        const int n = esp_http_client_read(client,
                                           reinterpret_cast<char*>(buf.get()),
                                           kReadChunk);
        if (n < 0) {
            ESP_LOGE(kTag, "http_client_read returned %d", n);
            ok = false;
            break;
        }
        if (n == 0) {
            // Server hung up before delivering the full Content-Length.
            ESP_LOGE(kTag, "early EOF at %u / %lld bytes",
                     static_cast<unsigned>(total_written),
                     static_cast<long long>(cl));
            ok = false;
            break;
        }
        std::string s = config::ota::handle_data_chunk(
            {buf.get(), static_cast<std::size_t>(n)});
        if (s.find("\"ok\":false") != std::string::npos) {
            ESP_LOGE(kTag, "handle_data_chunk: %s", s.c_str());
            ok = false;
            break;
        }
        total_written += static_cast<std::size_t>(n);
    }
    cleanup_client();

    if (!ok) {
        config::ota::abort_update();
        g_active.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    // Finalise — schedules a 500 ms reboot into the new image. We never
    // come back from that.
    std::string end_r = config::ota::handle_control_command(R"({"op":"end"})");
    ESP_LOGI(kTag, "ota end: %s", end_r.c_str());
    g_active.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace

const char* board_slug(std::uint8_t board_kind)
{
    // Order MUST match board::BoardKind: M5Base=0, TakaoBase=1, AtomNyan=2,
    // AtomS3=3, StopWatch=4. M5Base and TakaoBase both flash the cores3
    // build (same ESP32-S3 + servo bus pinning is a runtime detection
    // detail, not a per-binary one).
    switch (board_kind) {
    case 0:  return "cores3";    // M5Base
    case 1:  return "cores3";    // TakaoBase
    case 2:  return "atoms3r";   // AtomNyan
    case 3:  return "atoms3";    // AtomS3 slim
    case 4:  return "stopwatch"; // M5 StopWatch C152
    default: return nullptr;
    }
}

tl::expected<void, StartError> start(const std::string& tag,
                                      std::uint8_t board_kind)
{
    if (g_active.load(std::memory_order_acquire)) {
        return tl::unexpected(StartError::AlreadyRunning);
    }
    if (!tag_looks_safe(tag)) {
        return tl::unexpected(StartError::BadTag);
    }
    if (board_slug(board_kind) == nullptr) {
        return tl::unexpected(StartError::UnknownBoard);
    }

    auto* args = new WorkerArgs{tag, board_kind};
    g_active.store(true, std::memory_order_release);
    BaseType_t ok = xTaskCreate(&worker, "release-ota", kWorkerStack, args,
                                tskIDLE_PRIORITY + 3, nullptr);
    if (ok != pdPASS) {
        g_active.store(false, std::memory_order_release);
        delete args;
        return tl::unexpected(StartError::WorkerSpawnFailed);
    }
    return {};
}

bool active() { return g_active.load(std::memory_order_acquire); }

void request_abort()
{
    if (g_active.load(std::memory_order_acquire)) {
        g_abort.store(true, std::memory_order_release);
    }
}

namespace {

// Serve a single versions.json fetch from a dedicated worker task, waiting
// on a binary semaphore from the caller. mbedTLS handshake (~10 KiB peak)
// won't fit on the 6 KiB httpd stack, and running the fetch in-thread
// would blow it. Cache is 32 KiB max — versions.json is currently ~1 KiB
// and grows linearly with release count; cap keeps a malicious/misrouted
// response from eating our heap.
constexpr int kVersionsMaxBytes  = 32 * 1024;
constexpr int kVersionsTimeoutMs = 15000;

struct VersionsFetch {
    std::string       body;
    bool              ok;
    SemaphoreHandle_t done;
};

void versions_fetch_task(void* arg)
{
    auto* v = static_cast<VersionsFetch*>(arg);
    v->ok = false;

    esp_http_client_config_t cfg{};
    cfg.url = "https://ciniml.github.io/stackchan-idf/versions.json";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = kVersionsTimeoutMs;
    cfg.keep_alive_enable = false;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 4;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(kTag, "versions: client_init failed");
        xSemaphoreGive(v->done);
        vTaskDelete(nullptr);
    }

    do {
        int64_t cl = 0;
        const std::optional<int> status_opt = open_follow_redirects(client, cl);
        if (!status_opt.has_value()) {
            // open_follow_redirects already logged the concrete failure.
            break;
        }
        const int status = *status_opt;
        if (status != 200) {
            ESP_LOGE(kTag, "versions: HTTP %d", status);
            break;
        }
        if (cl <= 0 || cl > kVersionsMaxBytes) {
            ESP_LOGE(kTag, "versions: bad content length %lld", static_cast<long long>(cl));
            break;
        }
        v->body.resize(static_cast<std::size_t>(cl));
        int total = 0;
        while (total < cl) {
            const int n = esp_http_client_read(client,
                                               v->body.data() + total,
                                               cl - total);
            if (n <= 0) {
                ESP_LOGE(kTag, "versions: read %d at %d/%lld",
                         n, total, static_cast<long long>(cl));
                total = -1;
                break;
            }
            total += n;
        }
        if (total == cl) v->ok = true;
    } while (false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(v->done);
    vTaskDelete(nullptr);
}

} // namespace

bool fetch_versions_json(std::string& out)
{
    VersionsFetch v;
    v.ok = false;
    v.done = xSemaphoreCreateBinary();
    if (v.done == nullptr) return false;

    // 12 KiB internal-RAM stack — matches the release OTA worker's
    // headroom, since both do the same mbedTLS handshake + a small read
    // loop. No flash writes here, so a PSRAM stack would technically be
    // OK, but we keep it in internal RAM for consistency.
    BaseType_t ok = xTaskCreate(&versions_fetch_task, "vfetch", 12 * 1024, &v,
                                tskIDLE_PRIORITY + 3, nullptr);
    if (ok != pdPASS) {
        vSemaphoreDelete(v.done);
        return false;
    }
    // Wait long enough for the worker's own 15 s HTTP timeout + a bit of
    // handshake slack. If we return false while the worker is still going
    // we'd leak v — so we always wait.
    xSemaphoreTake(v.done, pdMS_TO_TICKS(kVersionsTimeoutMs + 5000));
    vSemaphoreDelete(v.done);
    if (v.ok) {
        out = std::move(v.body);
        return true;
    }
    return false;
}

} // namespace stackchan::wifi_config::release_ota
