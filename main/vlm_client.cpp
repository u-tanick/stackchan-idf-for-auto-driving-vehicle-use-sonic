// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "vlm_client.hpp"

#include <cstring>
#include <cstdio>
#include <vector>
#include <memory>
#include <atomic>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_camera.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include "wifi_sta.hpp"

namespace stackchan::app {

namespace {

constexpr const char* kTag = "vlm_client";

static std::string s_endpoint = VlmClient::kDefaultEndpoint;
static std::string s_model = VlmClient::kDefaultModel;
static std::string s_api_key;
static std::atomic<VlmStatus> s_status{VlmStatus::Unknown};
static std::atomic<bool> s_check_in_progress{false};
static uint32_t s_last_check_ms = 0;

struct HttpResponseContext {
    std::string body;
};

esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        auto* ctx = static_cast<HttpResponseContext*>(evt->user_data);
        if (ctx && evt->data && evt->data_len > 0) {
            ctx->body.append(static_cast<const char*>(evt->data), evt->data_len);
        }
    }
    return ESP_OK;
}

// JSON文字列値向けのエスケープヘルパー
std::string json_escape(const char* s)
{
    std::string out;
    if (!s) return out;
    while (*s) {
        if (*s == '"') {
            out += "\\\"";
        } else if (*s == '\\') {
            out += "\\\\";
        } else if (*s == '\n') {
            out += "\\n";
        } else if (*s == '\r') {
            out += "\\r";
        } else if (*s == '\t') {
            out += "\\t";
        } else {
            out += *s;
        }
        s++;
    }
    return out;
}

// レスポンス文字列から最初の '{' と 最後の '}' の間を抽出するヘルパー
std::string extract_json_block(const std::string& text)
{
    const auto start = text.find('{');
    const auto end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        return text.substr(start, end - start + 1);
    }
    return text;
}

} // namespace

void VlmClient::configure(std::string endpoint, std::string model, std::string api_key)
{
    if (!endpoint.empty()) s_endpoint = std::move(endpoint);
    if (!model.empty()) s_model = std::move(model);
    s_api_key = std::move(api_key);
    ESP_LOGI(kTag, "Configured VLM: endpoint=%s, model=%s, has_key=%d",
             s_endpoint.c_str(), s_model.c_str(), !s_api_key.empty());
}

VlmEvaluation VlmClient::evaluate_current_view(const char* direction_label)
{
    VlmEvaluation eval;
    eval.success = false;

    // 1. カメラフレーム取得
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
        ESP_LOGE(kTag, "Failed to capture camera frame");
        eval.reason = "Camera capture failed";
        return eval;
    }

    uint8_t* jpeg_buf = nullptr;
    size_t jpeg_len = 0;
    bool need_free_jpeg = false;

    if (fb->format == PIXFORMAT_JPEG) {
        jpeg_buf = fb->buf;
        jpeg_len = fb->len;
    } else if (fb->format == PIXFORMAT_RGB565) {
        if (!fmt2jpg(fb->buf, fb->len, fb->width, fb->height, PIXFORMAT_RGB565, 60, &jpeg_buf, &jpeg_len)) {
            ESP_LOGE(kTag, "fmt2jpg conversion failed");
            esp_camera_fb_return(fb);
            eval.reason = "JPEG conversion failed";
            return eval;
        }
        need_free_jpeg = true;
    } else {
        ESP_LOGE(kTag, "Unsupported camera format: %d", fb->format);
        esp_camera_fb_return(fb);
        eval.reason = "Unsupported camera format";
        return eval;
    }

    // 2. base64 エンコード (PSRAM を活用)
    size_t b64_len = 0;
    (void)mbedtls_base64_encode(nullptr, 0, &b64_len, jpeg_buf, jpeg_len);
    
    char* b64_buf = static_cast<char*>(heap_caps_malloc(b64_len + 1, MALLOC_CAP_SPIRAM));
    if (b64_buf == nullptr) {
        ESP_LOGE(kTag, "Failed to allocate PSRAM for base64 (%u bytes)", static_cast<unsigned>(b64_len));
        if (need_free_jpeg && jpeg_buf) free(jpeg_buf);
        esp_camera_fb_return(fb);
        eval.reason = "Out of memory (PSRAM)";
        return eval;
    }

    size_t written = 0;
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(b64_buf), b64_len + 1, &written, jpeg_buf, jpeg_len);
    b64_buf[written] = '\0';

    // カメラフレームと一時JPEGの返却・解放
    if (need_free_jpeg && jpeg_buf) free(jpeg_buf);
    esp_camera_fb_return(fb);

    // 3. プロンプトと JSON リクエストペイロードの作成
    // PSRAM 上に文字列を構築
    char prompt_raw[512];
    std::snprintf(prompt_raw, sizeof(prompt_raw),
                  "幅10cmの小型ロボットの自律走行ナビ判断です。画像内の向き(%s)について、直前50cm〜1mの床面に走行スペースがあるか判定し、次のJSON形式のみで出力してください: "
                  "{\"passable\": trueまたはfalse, \"score\": 0〜100, \"reason\": \"理由\"} "
                  "※直前50cm以内に壁や箱等の障害物が密着していなければpassable:true(score:60〜100)とし、遠くの壁や家具、薄暗さは無視してください。",
                  direction_label ? direction_label : "正面");
    const std::string prompt_escaped = json_escape(prompt_raw);

    std::string payload;
    payload.reserve(written + 768);
    payload += "{\"model\":\"";
    payload += s_model;
    payload += "\",\"messages\":[{\"role\":\"user\",\"content\":[";
    payload += "{\"type\":\"text\",\"text\":\"";
    payload += prompt_escaped;
    payload += "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    payload += b64_buf;
    payload += "\"}}]}],\"max_tokens\":120,\"temperature\":0.1}";

    // base64 バッファはもう不要なので即座に解放
    heap_caps_free(b64_buf);

    ESP_LOGI(kTag, "Sending evaluation request to VLM for direction '%s' (payload size: %u bytes)...",
             direction_label, static_cast<unsigned>(payload.size()));

    // 4. HTTP POST 送信
    HttpResponseContext resp_ctx;
    esp_http_client_config_t http_cfg{};
    http_cfg.url = s_endpoint.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = 25000;
    http_cfg.buffer_size = 2048;
    http_cfg.buffer_size_tx = 2048;
    http_cfg.event_handler = http_event_handler;
    http_cfg.user_data = &resp_ctx;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == nullptr) {
        ESP_LOGE(kTag, "Failed to initialize esp_http_client");
        eval.reason = "HTTP client init failed";
        return eval;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (!s_api_key.empty()) {
        std::string auth = "Bearer " + s_api_key;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }

    esp_err_t err = esp_http_client_open(client, payload.size());
    if (err == ESP_OK) {
        int wlen = esp_http_client_write(client, payload.data(), payload.size());
        if (wlen < 0) {
            ESP_LOGE(kTag, "Failed to write HTTP payload");
            err = ESP_FAIL;
        } else {
            esp_http_client_fetch_headers(client);
            int status_code = esp_http_client_get_status_code(client);
            if (status_code >= 200 && status_code < 300) {
                // 残りのボディを読み込む
                while (true) {
                    char temp[512];
                    int r = esp_http_client_read(client, temp, sizeof(temp));
                    if (r <= 0) break;
                    resp_ctx.body.append(temp, r);
                }
            } else {
                ESP_LOGE(kTag, "HTTP POST failed with status %d", status_code);
                err = ESP_FAIL;
            }
        }
    } else {
        ESP_LOGE(kTag, "esp_http_client_open failed: %s", esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || resp_ctx.body.empty()) {
        eval.reason = "VLM request failed or empty response";
        return eval;
    }

    ESP_LOGI(kTag, "VLM Response received (%u bytes)", static_cast<unsigned>(resp_ctx.body.size()));

    // 5. レスポンス JSON のパース
    cJSON* root = cJSON_Parse(resp_ctx.body.c_str());
    if (root == nullptr) {
        ESP_LOGE(kTag, "Failed to parse outer response JSON");
        eval.reason = "Invalid response JSON";
        return eval;
    }

    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    cJSON* choice0 = (choices && cJSON_GetArraySize(choices) > 0) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    cJSON* message = choice0 ? cJSON_GetObjectItem(choice0, "message") : nullptr;
    cJSON* content = message ? cJSON_GetObjectItem(message, "content") : nullptr;

    if (content && content->valuestring) {
        const std::string content_str = content->valuestring;
        ESP_LOGI(kTag, "Model content: %s", content_str.c_str());

        const std::string json_str = extract_json_block(content_str);
        cJSON* eval_json = cJSON_Parse(json_str.c_str());
        if (eval_json != nullptr) {
            cJSON* passable_item = cJSON_GetObjectItem(eval_json, "passable");
            cJSON* score_item = cJSON_GetObjectItem(eval_json, "score");
            cJSON* reason_item = cJSON_GetObjectItem(eval_json, "reason");

            if (cJSON_IsBool(passable_item)) {
                eval.passable = cJSON_IsTrue(passable_item);
            }
            if (cJSON_IsNumber(score_item)) {
                eval.score = score_item->valueint;
            }
            if (cJSON_IsString(reason_item) && reason_item->valuestring) {
                eval.reason = reason_item->valuestring;
            }
            eval.success = true;
            cJSON_Delete(eval_json);
        } else {
            ESP_LOGW(kTag, "Failed to parse inner evaluation JSON from content");
            eval.reason = content_str;
            eval.success = false;
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(kTag, "Evaluation for '%s': success=%d, passable=%d, score=%d, reason='%s'",
             direction_label, eval.success, eval.passable, eval.score, eval.reason.c_str());

    // 評価結果によりステータスを追従更新
    if (eval.success) {
        s_status.store(VlmStatus::Available, std::memory_order_relaxed);
    }

    return eval;
}

namespace {

void vlm_health_check_task(void* arg)
{
    s_status.store(VlmStatus::Checking, std::memory_order_relaxed);
    ESP_LOGI(kTag, "Starting VLM health check probe to %s ...", s_endpoint.c_str());

    HttpResponseContext resp_ctx;
    esp_http_client_config_t http_cfg{};
    http_cfg.url = s_endpoint.c_str();
    http_cfg.method = HTTP_METHOD_POST;
    http_cfg.timeout_ms = 3000;
    http_cfg.buffer_size = 512;
    http_cfg.buffer_size_tx = 512;
    http_cfg.event_handler = http_event_handler;
    http_cfg.user_data = &resp_ctx;

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    bool ok = false;
    if (client != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (!s_api_key.empty()) {
            std::string auth = "Bearer " + s_api_key;
            esp_http_client_set_header(client, "Authorization", auth.c_str());
        }

        char payload[160];
        std::snprintf(payload, sizeof(payload),
                      "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1}",
                      s_model.c_str());

        esp_err_t err = esp_http_client_open(client, std::strlen(payload));
        if (err == ESP_OK) {
            int wlen = esp_http_client_write(client, payload, std::strlen(payload));
            if (wlen >= 0) {
                esp_http_client_fetch_headers(client);
                int status_code = esp_http_client_get_status_code(client);
                ESP_LOGI(kTag, "VLM probe response HTTP status: %d", status_code);
                // 200 OK、または 2xx/3xx/400 (model responded) ならサーバー稼働と判定
                if (status_code >= 200 && status_code < 500) {
                    ok = true;
                }
            }
        } else {
            ESP_LOGW(kTag, "VLM probe connection failed: %s (%s)", esp_err_to_name(err), s_endpoint.c_str());
        }
        esp_http_client_cleanup(client);
    }

    s_status.store(ok ? VlmStatus::Available : VlmStatus::Unavailable, std::memory_order_relaxed);
    ESP_LOGI(kTag, "VLM health check finished: %s", ok ? "AVAILABLE" : "UNAVAILABLE");
    s_check_in_progress.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

} // namespace

VlmStatus VlmClient::get_status()
{
    if (!wifi_is_connected()) {
        return VlmStatus::Unavailable;
    }
    return s_status.load(std::memory_order_relaxed);
}

void VlmClient::trigger_health_check(bool force_recheck)
{
    if (!wifi_is_connected()) {
        s_status.store(VlmStatus::Unavailable, std::memory_order_relaxed);
        return;
    }
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const auto current = s_status.load(std::memory_order_relaxed);
    if (!force_recheck && current != VlmStatus::Unknown) {
        // 前回チェックから 8 秒以内なら再チェックを抑止
        if (now_ms - s_last_check_ms < 8000) {
            return;
        }
    }
    bool expected = false;
    if (s_check_in_progress.compare_exchange_strong(expected, true)) {
        s_last_check_ms = now_ms;
        s_status.store(VlmStatus::Checking, std::memory_order_relaxed);
        xTaskCreatePinnedToCore(&vlm_health_check_task, "vlm_probe", 6144, nullptr, 2, nullptr, 0);
    }
}

} // namespace stackchan::app
