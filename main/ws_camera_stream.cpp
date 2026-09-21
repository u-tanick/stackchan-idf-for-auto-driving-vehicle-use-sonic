// SPDX-License-Identifier: MIT
// PCブラウザ向け WebSocket カメラ映像ストリーミングサーバー (custom_141 互換)

#include "ws_camera_stream.hpp"

#include <atomic>
#include <vector>
#include <mutex>
#include <cstring>
#include <arpa/inet.h>

#include <esp_log.h>
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

namespace stackchan::app::ws_camera {

namespace {

constexpr const char* kTag = "ws-cam";

enum class DataType : uint8_t {
    Opus              = 0x01,
    Jpeg              = 0x02,
    ControlAvatar     = 0x03,
    ControlMotion     = 0x04,
    StartCameraStream = 0x05,
    StopCameraStream  = 0x06,
};

httpd_handle_t g_server = nullptr;
std::mutex g_send_mutex;
std::atomic<bool> g_streaming{false};
TaskHandle_t g_task_handle = nullptr;

void send_packet(DataType type, const uint8_t* data, size_t len)
{
    std::lock_guard<std::mutex> lock(g_send_mutex);
    if (g_server == nullptr) return;

    std::vector<uint8_t> packet;
    packet.reserve(1 + 4 + len);
    packet.push_back(static_cast<uint8_t>(type));

    uint32_t net_len = htonl(static_cast<uint32_t>(len));
    const auto* len_ptr = reinterpret_cast<const uint8_t*>(&net_len);
    packet.push_back(len_ptr[0]);
    packet.push_back(len_ptr[1]);
    packet.push_back(len_ptr[2]);
    packet.push_back(len_ptr[3]);

    if (len > 0 && data != nullptr) {
        packet.insert(packet.end(), data, data + len);
    }

    httpd_ws_frame_t ws_pkt;
    std::memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = packet.data();
    ws_pkt.len = packet.size();
    ws_pkt.type = HTTPD_WS_TYPE_BINARY;

    // ブロードキャスト: 全アクティブクライアントに一斉送信
    size_t max_clients = 4;
    int client_fds[4] = {0};
    if (httpd_get_client_list(g_server, &max_clients, client_fds) == ESP_OK) {
        for (size_t i = 0; i < max_clients; i++) {
            if (httpd_ws_get_fd_info(g_server, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                httpd_ws_send_frame_async(g_server, client_fds[i], &ws_pkt);
            }
        }
    }
}

void stream_task_entry(void* /*arg*/)
{
    ESP_LOGI(kTag, "stream task started");
    while (true) {
        if (!g_streaming.load(std::memory_order_acquire)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#if CONFIG_STACKCHAN_CAMERA_ENABLED
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb != nullptr) {
            uint8_t* jpeg_buf = nullptr;
            size_t jpeg_len = 0;

            if (fb->format == PIXFORMAT_JPEG) {
                // すでにJPEG形式の場合
                send_packet(DataType::Jpeg, fb->buf, fb->len);
            } else if (fb->format == PIXFORMAT_RGB565) {
                // RGB565 から JPEG に変換 (品質: 65)
                if (fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                            PIXFORMAT_RGB565, 65, &jpeg_buf, &jpeg_len)) {
                    send_packet(DataType::Jpeg, jpeg_buf, jpeg_len);
                    free(jpeg_buf);
                }
            } else if (fb->format == PIXFORMAT_GRAYSCALE) {
                // Grayscale から JPEG に変換
                if (fmt2jpg(fb->buf, fb->len, fb->width, fb->height,
                            PIXFORMAT_GRAYSCALE, 65, &jpeg_buf, &jpeg_len)) {
                    send_packet(DataType::Jpeg, jpeg_buf, jpeg_len);
                    free(jpeg_buf);
                }
            }
            esp_camera_fb_return(fb);
        }
#endif
        // custom_141 仕様の配信周期: 約350ms (約3fps)
        vTaskDelay(pdMS_TO_TICKS(350));
    }
}

esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(kTag, "WebSocket client handshake complete (fd: %d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    std::memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(kTag, "httpd_ws_recv_frame failed: %d", ret);
        return ret;
    }

    if (ws_pkt.len > 0) {
        std::vector<uint8_t> buf(ws_pkt.len);
        ws_pkt.payload = buf.data();
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            if (buf.size() >= 1) {
                auto type = static_cast<DataType>(buf[0]);
                if (type == DataType::StartCameraStream) {
                    ESP_LOGI(kTag, "StartCameraStream command received from PC");
                    set_streaming(true);
                } else if (type == DataType::StopCameraStream) {
                    ESP_LOGI(kTag, "StopCameraStream command received from PC");
                    set_streaming(false);
                }
            }
        }
    }
    return ret;
}

} // namespace

esp_err_t register_ws_handler(httpd_handle_t server)
{
    if (server == nullptr) return ESP_ERR_INVALID_ARG;
    g_server = server;

    httpd_uri_t ws_uri = {};
    ws_uri.uri        = "/ws";
    ws_uri.method     = HTTP_GET;
    ws_uri.handler    = ws_handler;
    ws_uri.user_ctx   = nullptr;
    ws_uri.is_websocket = true;
    ws_uri.handle_ws_control_frames = false;
    ws_uri.supported_subprotocol = nullptr;

    esp_err_t err = httpd_register_uri_handler(server, &ws_uri);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "Registered WebSocket endpoint: /ws");
        start_stream_task();
    } else {
        ESP_LOGE(kTag, "Failed to register /ws handler: %s", esp_err_to_name(err));
    }
    return err;
}

void set_streaming(bool enabled)
{
    g_streaming.store(enabled, std::memory_order_release);
    ESP_LOGI(kTag, "Camera streaming %s", enabled ? "ENABLED" : "DISABLED");
}

bool is_streaming()
{
    return g_streaming.load(std::memory_order_acquire);
}

void start_stream_task()
{
    if (g_task_handle == nullptr) {
        xTaskCreatePinnedToCore(&stream_task_entry, "ws-cam-stream", 4 * 1024,
                                nullptr, tskIDLE_PRIORITY + 1, &g_task_handle, 1);
    }
}

void stop_stream_task()
{
    set_streaming(false);
    if (g_task_handle != nullptr) {
        vTaskDelete(g_task_handle);
        g_task_handle = nullptr;
    }
}

} // namespace stackchan::app::ws_camera
