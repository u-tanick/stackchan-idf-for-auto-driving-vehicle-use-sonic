// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Phase 1 of the Telegram Bot integration. The one-shot getUpdates probe
// here exists so we can verify three things end-to-end before building
// any task/state machine on top:
//   1. HTTPS connectivity to api.telegram.org from STA (TLS handshake
//      with the embedded ISRG Root X1 is the failure-prone bit)
//   2. The bot token shape is accepted (Telegram returns 401 if it's bad,
//      `ok:true` if it's good)
//   3. JSON parsing on the response landed correctly (cJSON handles the
//      few KB Telegram returns without issues, but worth confirming)
//
// Once that's confirmed we replace this with a polling task + sink, per
// PLAN.md §2.

#include "telegram/telegram.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

namespace stackchan::telegram {

namespace {

constexpr const char* kTag = "telegram";
// Telegram caps a single response payload at ~16 MiB but getUpdates with
// limit=10 and a 5 s timeout never gets close. 16 KiB covers a handful of
// medium-length messages with plenty of slack; anything larger we'd treat
// as a failure (see Error::Truncated).
constexpr std::size_t kBodyBufBytes = 16 * 1024;

struct ResponseBuf {
    char* data;       // owned, kBodyBufBytes bytes
    std::size_t len;  // written so far
    bool truncated;   // set when a chunk would overflow `data`
};

// esp_http_client event handler — appends each chunk to ResponseBuf and
// flags truncation. We avoid the streaming JSON parser route (which
// would let us start parsing before the response finishes) because the
// payloads are small and we want the entire `ok` flag visible before any
// downstream action.
esp_err_t on_http_event(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    auto* rb = static_cast<ResponseBuf*>(evt->user_data);
    if (rb == nullptr || rb->data == nullptr) return ESP_OK;
    if (rb->truncated) return ESP_OK; // already overflowed, ignore the rest
    if (rb->len + static_cast<std::size_t>(evt->data_len) >= kBodyBufBytes) {
        rb->truncated = true;
        return ESP_OK;
    }
    std::memcpy(rb->data + rb->len, evt->data, evt->data_len);
    rb->len += evt->data_len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

} // namespace

const char* to_string(Error e) noexcept
{
    switch (e) {
    case Error::InvalidToken: return "invalid token";
    case Error::HttpInit: return "http_client_init failed";
    case Error::HttpPerform: return "http_perform failed";
    case Error::BadResponse: return "bad response (parse / ok:false)";
    case Error::Truncated: return "response truncated";
    }
    return "?";
}

tl::expected<std::int64_t, Error>
get_updates_one_shot(std::string_view token, std::int64_t offset, int timeout_sec)
{
    // Cheap sanity check — Telegram tokens are <digits>:<letters>, ~46 char.
    // We don't validate the digits/letters split here, just the colon.
    if (token.empty() || token.find(':') == std::string_view::npos) {
        ESP_LOGE(kTag, "invalid token (empty or no ':')");
        return tl::unexpected{Error::InvalidToken};
    }

    // Build the request URL. We could pass offset/timeout/limit/
    // allowed_updates as query string, but a POST with a JSON body is more
    // consistent with how we'll do it later, and avoids URL escaping fiddly
    // bits. esp_http_client supports both equally; sticking to POST.
    std::string url = "https://api.telegram.org/bot";
    url.append(token);
    url.append("/getUpdates");

    char body[160];
    const int body_len = std::snprintf(
        body, sizeof(body),
        R"({"offset":%lld,"timeout":%d,"limit":10,"allowed_updates":["message"]})",
        static_cast<long long>(offset), timeout_sec);

    // 16 KiB body buffer lives on the heap, not stack — telegram is invoked
    // from app_main's 8 KiB stack and we don't want to chew through half of
    // it on the response buffer.
    ResponseBuf rb{};
    rb.data = static_cast<char*>(std::malloc(kBodyBufBytes));
    if (rb.data == nullptr) {
        ESP_LOGE(kTag, "malloc(%zu) failed for response buffer", kBodyBufBytes);
        return tl::unexpected{Error::HttpInit};
    }
    rb.data[0] = '\0';

    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_POST;
    // api.telegram.org is currently chained under Go Daddy Root CA G2 (NOT
    // Let's Encrypt). Using the Mozilla bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y,
    // which is enabled in our build) covers Go Daddy + every other root the
    // upstream service might swap to, at the cost of ~12 KiB of flash that's
    // already paid since the bundle is in the build regardless.
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = on_http_event;
    cfg.user_data = &rb;
    // 5 s on top of the long-poll timeout so the TLS handshake and JSON
    // delivery have headroom. esp_http_client treats this as the *socket*
    // timeout, not the total operation timeout, so it's per-recv.
    cfg.timeout_ms = (timeout_sec + 5) * 1000;
    cfg.skip_cert_common_name_check = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        std::free(rb.data);
        ESP_LOGE(kTag, "esp_http_client_init failed");
        return tl::unexpected{Error::HttpInit};
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, body_len);

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    const int content_len = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_http_client_perform: %s", esp_err_to_name(err));
        std::free(rb.data);
        return tl::unexpected{Error::HttpPerform};
    }
    if (rb.truncated) {
        ESP_LOGE(kTag, "response truncated (>%zu B)", kBodyBufBytes);
        std::free(rb.data);
        return tl::unexpected{Error::Truncated};
    }
    ESP_LOGI(kTag, "HTTP %d, %d bytes received (Content-Length=%d)",
             status, (int)rb.len, content_len);
    if (status < 200 || status >= 300) {
        // 401 = bad token, 400 = bad request body, etc. Log the body so the
        // user can see Telegram's diagnostic message.
        ESP_LOGE(kTag, "non-2xx body: %s", rb.data);
        std::free(rb.data);
        return tl::unexpected{Error::HttpPerform};
    }

    // Parse the response. Shape:
    //   {"ok": true, "result": [ { "update_id": N, "message": {...} }, ... ]}
    cJSON* root = cJSON_Parse(rb.data);
    if (root == nullptr) {
        ESP_LOGE(kTag, "cJSON_Parse failed; raw body follows");
        ESP_LOGE(kTag, "%s", rb.data);
        std::free(rb.data);
        return tl::unexpected{Error::BadResponse};
    }
    std::free(rb.data); // raw body no longer needed
    rb.data = nullptr;

    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        const cJSON* desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        ESP_LOGE(kTag, "telegram returned ok:false (%s)",
                 cJSON_IsString(desc) ? desc->valuestring : "(no description)");
        cJSON_Delete(root);
        return tl::unexpected{Error::BadResponse};
    }

    const cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsArray(result)) {
        ESP_LOGE(kTag, "result is not an array");
        cJSON_Delete(root);
        return tl::unexpected{Error::BadResponse};
    }

    const int n = cJSON_GetArraySize(result);
    ESP_LOGI(kTag, "ok — %d update(s) returned", n);

    std::int64_t max_update_id = offset - 1;
    for (int i = 0; i < n; ++i) {
        const cJSON* upd = cJSON_GetArrayItem(result, i);
        const cJSON* uid = cJSON_GetObjectItemCaseSensitive(upd, "update_id");
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(upd, "message");
        if (cJSON_IsNumber(uid) && uid->valuedouble > max_update_id) {
            max_update_id = static_cast<std::int64_t>(uid->valuedouble);
        }
        if (msg == nullptr) continue;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(msg, "text");
        const cJSON* chat = cJSON_GetObjectItemCaseSensitive(msg, "chat");
        const cJSON* chat_id = chat ? cJSON_GetObjectItemCaseSensitive(chat, "id") : nullptr;
        const cJSON* from = cJSON_GetObjectItemCaseSensitive(msg, "from");
        const cJSON* uname = from ? cJSON_GetObjectItemCaseSensitive(from, "username") : nullptr;
        ESP_LOGI(kTag, "  [%d] chat=%lld from=%s text=%.120s",
                 i,
                 cJSON_IsNumber(chat_id) ? (long long)chat_id->valuedouble : 0LL,
                 cJSON_IsString(uname) ? uname->valuestring : "(unknown)",
                 cJSON_IsString(text) ? text->valuestring : "(non-text update)");
    }

    cJSON_Delete(root);
    return max_update_id;
}

} // namespace stackchan::telegram
