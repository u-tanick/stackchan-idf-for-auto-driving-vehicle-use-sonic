// SPDX-License-Identifier: MIT
// PCブラウザ向け WebSocket カメラ映像ストリーミングサーバー (custom_141 互換)

#pragma once

#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>
#include <cstddef>

namespace stackchan::app::ws_camera {

// WebSocket URI ハンドラを既存の HTTP サーバーに登録する (/ws)
esp_err_t register_ws_handler(httpd_handle_t server);

// ストリーミング配信の有効/無効
void set_streaming(bool enabled);
bool is_streaming();

// 定期配信タスクの開始・停止
void start_stream_task();
void stop_stream_task();

} // namespace stackchan::app::ws_camera
