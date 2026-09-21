// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"

#include <cstdio>
#include <cmath>
#include "avatar/expression.hpp"
#include "vlm_client.hpp"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {
constexpr const char* kTag = "atomic_client";
constexpr uint32_t kI2cFreq = 100000;

bool s_initialized = false;

SharedState::Driving::Mode s_last_synced_mode = SharedState::Driving::Mode::Autonomous;
bool s_mode_force_sync = true;
uint32_t s_last_tick_ms = 0;

enum class AutoDriveState {
    InitWait,       // 起動後の待機（サーボ初期診断完了待ち）
    Forward,        // 前進走行中
    BackingUp,      // 10cm未満時の微速後退
    StartScan,      // 探索シーケンス開始（首振り開始）
    ScanWait,       // 首振り静止待ち（1秒）
    ScanEvaluate,   // 撮影 & VLM評価
    Decision,       // 全4方向の評価結果集計 & 最善方向決定
    Turning,        // IMUジャイロ積分による旋回中
    VerifyPath,     // 旋回完了後の進路確認
};

struct ScanPoint {
    float yaw_deg;
    const char* name;
    const char* msg;
};

static const ScanPoint kScanPoints[4] = {
    { -90.0f, "右(90度)", "右(90°)を見てみるね…" },
    { -45.0f, "右斜め(45度)", "右斜め(45°)を見てみるね…" },
    { +90.0f, "左(90度)", "左(90°)を見てみるね…" },
    { +45.0f, "左斜め(45度)", "左斜め(45°)を見てみるね…" }
};

AutoDriveState s_drive_state = AutoDriveState::InitWait;
uint32_t s_state_start_ms = 0;
int s_scan_index = 0;
VlmEvaluation s_scan_evals[4];

// IMU旋回用
float s_target_turn_deg = 0.0f;
bool s_turn_spin_right = true;
float s_turn_integrated_deg = 0.0f;
int64_t s_turn_last_us = 0;
} // namespace

esp_err_t AtomicMotionClient::init(int sda_pin, int scl_pin)
{
    if (s_initialized) {
        return ESP_OK;
    }

    // Always call begin() to configure GPIOs and start I2C peripheral
    M5.Ex_I2C.begin(I2C_NUM_0, sda_pin, scl_pin);

    // Scan all 7-bit addresses to see what responds on Port A
    bool found_any = false;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (M5.Ex_I2C.scanID(addr, kI2cFreq)) {
            ESP_LOGI(kTag, "I2C device found on Port A at 0x%02X", addr);
            found_any = true;
        }
    }
    if (!found_any) {
        ESP_LOGW(kTag, "I2C scan finished: NO devices responded on Port A (SDA:%d, SCL:%d)", sda_pin, scl_pin);
    }

    s_initialized = true;
    s_mode_force_sync = true;
    return ESP_OK;
}

esp_err_t AtomicMotionClient::set_mode(SharedState::Driving::Mode mode)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    const uint8_t val = (mode == SharedState::Driving::Mode::Manual) ? 0x01 : 0x00;
    if (M5.Ex_I2C.writeRegister8(kDefaultSlaveAddr, 0x04, val, kI2cFreq)) {
        ESP_LOGI(kTag, "Robot mode synchronized to %s", (mode == SharedState::Driving::Mode::Manual) ? "Manual" : "Autonomous");
        return ESP_OK;
    } else {
        static int64_t last_err_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_err_us > 3000000) {
            last_err_us = now_us;
            ESP_LOGW(kTag, "Failed to send set_mode (AtomS3 connected on Port A?)");
        }
        return ESP_FAIL;
    }
}

esp_err_t AtomicMotionClient::send_command(Command cmd)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return M5.Ex_I2C.writeRegister8(kDefaultSlaveAddr, 0x00, static_cast<uint8_t>(cmd), kI2cFreq) ? ESP_OK : ESP_FAIL;
}

esp_err_t AtomicMotionClient::set_motor_speeds(int8_t left, int8_t right)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    const uint8_t buf[2] = {static_cast<uint8_t>(left), static_cast<uint8_t>(right)};
    return M5.Ex_I2C.writeRegister(kDefaultSlaveAddr, 0x01, buf, sizeof(buf), kI2cFreq) ? ESP_OK : ESP_FAIL;
}

esp_err_t AtomicMotionClient::set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    const uint8_t buf[3] = {r, g, b};
    return M5.Ex_I2C.writeRegister(kDefaultSlaveAddr, 0x20, buf, sizeof(buf), kI2cFreq) ? ESP_OK : ESP_FAIL;
}

esp_err_t AtomicMotionClient::read_status(Status& out_status)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    // Read distance (0x10, 2 bytes), flags (0x12, 1 byte)
    // Send register address with explicit STOP so Slave's onReceive fires before onRequest
    uint8_t data[3] = {0};
    if (M5.Ex_I2C.start(kDefaultSlaveAddr, false, kI2cFreq) &&
        M5.Ex_I2C.write(0x10) &&
        M5.Ex_I2C.stop() &&
        M5.Ex_I2C.start(kDefaultSlaveAddr, true, kI2cFreq) &&
        M5.Ex_I2C.read(data, 3) &&
        M5.Ex_I2C.stop()) {
        out_status.distance_mm = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        out_status.obstacle_flags = data[2];
    } else {
        M5.Ex_I2C.stop();
        return ESP_FAIL;
    }

    static uint32_t s_last_raw_log_ms = 0;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (now_ms - s_last_raw_log_ms >= 1000) {
        s_last_raw_log_ms = now_ms;
        ESP_LOGI(kTag, "raw 0x10 read: [0x%02X, 0x%02X, 0x%02X] -> dist=%u mm",
                 data[0], data[1], data[2], out_status.distance_mm);
    }

    // Read JoyC active flag (0x05)
    uint8_t joy_byte = 0;
    if (M5.Ex_I2C.start(kDefaultSlaveAddr, false, kI2cFreq) &&
        M5.Ex_I2C.write(0x05) &&
        M5.Ex_I2C.stop() &&
        M5.Ex_I2C.start(kDefaultSlaveAddr, true, kI2cFreq) &&
        M5.Ex_I2C.read(&joy_byte, 1) &&
        M5.Ex_I2C.stop()) {
        out_status.joy_active = (joy_byte != 0);
    } else {
        M5.Ex_I2C.stop();
    }

    return ESP_OK;
}

void AtomicMotionClient::tick(SharedState& state)
{
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    if (!s_initialized) {
        static uint32_t s_last_init_attempt_ms = 0;
        if (now_ms - s_last_init_attempt_ms < 2000) {
            return;
        }
        s_last_init_attempt_ms = now_ms;
        init();
        if (!s_initialized) return;
    }

    if (now_ms - s_last_tick_ms < 50) { // 20 Hz sync
        return;
    }
    s_last_tick_ms = now_ms;

    const auto current_mode = state.driving.mode.load(std::memory_order_relaxed);

    // Sync mode if changed or boot
    if (s_mode_force_sync || current_mode != s_last_synced_mode) {
        if (set_mode(current_mode) == ESP_OK) {
            s_last_synced_mode = current_mode;
            s_mode_force_sync = false;
        }
    }

    // Read sensor telemetry
    Status status;
    if (read_status(status) == ESP_OK) {
        state.driving.distance_mm.store(status.distance_mm, std::memory_order_relaxed);
        state.driving.obstacle_flags.store(status.obstacle_flags, std::memory_order_relaxed);
        state.driving.joy_active.store(status.joy_active, std::memory_order_relaxed);

        static uint32_t s_last_telemetry_log_ms = 0;
        if (now_ms - s_last_telemetry_log_ms >= 1000) {
            s_last_telemetry_log_ms = now_ms;
            ESP_LOGI(kTag, "Telemetry: dist=%u mm, obs=0x%02X, joy=%d",
                     status.distance_mm, status.obstacle_flags, status.joy_active);
        }

        // 障害物検知時の画面演出（フキダシ・セリフ・表情フィードバック）
        // ※左右確認・探索・旋回モード中は超音波センサーの演出をOFFにし、探索のセリフに専念させる
        const bool is_scanning_or_turning = (current_mode == SharedState::Driving::Mode::Autonomous &&
                                             s_drive_state != AutoDriveState::Forward &&
                                             s_drive_state != AutoDriveState::InitWait);

        static bool s_last_obstacle = false;
        static uint32_t s_last_balloon_update_ms = 0;

        const bool obstacle = (status.obstacle_flags & 0x01) != 0;
        const uint16_t dist_cm = status.distance_mm / 10;

        if (!is_scanning_or_turning) {
            if (obstacle) {
                // 新規検知または検知継続中の定期更新（2秒ごと）
                if (!s_last_obstacle || (now_ms - s_last_balloon_update_ms >= 2000)) {
                    s_last_balloon_update_ms = now_ms;
                    char msg[64];
                    if ((status.obstacle_flags & 0x04) || (status.distance_mm > 0 && status.distance_mm < 100)) {
                        snprintf(msg, sizeof(msg), "ぶつかるー！(%u cm)", dist_cm);
                        state.face.expression.store(static_cast<int>(avatar::Expression::Angry), std::memory_order_relaxed);
                    } else {
                        snprintf(msg, sizeof(msg), "障害物接近中！(%u cm)", dist_cm);
                        state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    }
                    state.set_balloon_text(msg, 2000);
                }
            } else if (s_last_obstacle) {
                // 障害物がなくなった時
                state.set_balloon_text("よし、クリア！", 1500);
                state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
            }
        }
        s_last_obstacle = obstacle;
    }

    // If Autonomous, run VLM & IMU based obstacle avoidance state machine
    if (current_mode == SharedState::Driving::Mode::Autonomous) {
        const uint16_t dist_mm = state.driving.distance_mm.load(std::memory_order_relaxed);

        switch (s_drive_state) {
        case AutoDriveState::InitWait:
            // 起動後 15 秒（サーボセルフテスト完了）待機してから前進開始
            if (now_ms >= 15000 && dist_mm > 0) {
                state.set_balloon_text("前進スタート！", 1500);
                state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                send_command(CmdForward);
                s_drive_state = AutoDriveState::Forward;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::Forward:
            send_command(CmdForward);
            // 10cm 以内に壁があれば停止
            if (dist_mm > 0 && dist_mm <= 100) {
                send_command(CmdStop);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "おっと！壁だよ！（%u cm）", dist_mm / 10);
                state.set_balloon_text(buf, 2000);
                state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);

                if (dist_mm < 90) {
                    // 10cm未満まで近接してしまったら後退へ
                    s_drive_state = AutoDriveState::BackingUp;
                    s_state_start_ms = now_ms;
                } else {
                    // 10cm付近で停止、首振り探索へ
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                }
            }
            break;

        case AutoDriveState::BackingUp:
            // 10cm に達するまで微速後退
            send_command(CmdBackward);
            if (dist_mm >= 100 || (now_ms - s_state_start_ms >= 1500)) {
                send_command(CmdStop);
                state.set_balloon_text("10cmまで下がったよ", 1500);
                s_drive_state = AutoDriveState::StartScan;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::StartScan:
            // 500ms 静止を待ってから首振り開始
            if (now_ms - s_state_start_ms >= 500) {
                s_scan_index = 0;
                ESP_LOGI(kTag, "Start scan sequence. Target 0: %s (yaw=%.1f deg)",
                         kScanPoints[0].name, kScanPoints[0].yaw_deg);
                state.servo.speed_override.store(250, std::memory_order_relaxed);
                state.servo.target_yaw_deg.store(kScanPoints[0].yaw_deg, std::memory_order_relaxed);
                state.set_balloon_text(kScanPoints[0].msg, 2000);
                s_drive_state = AutoDriveState::ScanWait;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::ScanWait:
            // 首振り後、手ブレ防止のため 1 秒静止
            if (now_ms - s_state_start_ms >= 1000) {
                s_drive_state = AutoDriveState::ScanEvaluate;
            }
            break;

        case AutoDriveState::ScanEvaluate: {
            // 現在のカメラフレームを VLM で評価
            const auto& pt = kScanPoints[s_scan_index];
            ESP_LOGI(kTag, "Evaluating view for %s...", pt.name);
            VlmEvaluation eval = VlmClient::evaluate_current_view(pt.name);
            s_scan_evals[s_scan_index] = eval;

            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s: %d点 (%s)", pt.name, eval.score, eval.passable ? "OK" : "NG");
            state.set_balloon_text(buf, 2000);

            s_scan_index++;
            if (s_scan_index < 4) {
                // 次の方向へ首を向ける
                ESP_LOGI(kTag, "Next scan target %d: %s (yaw=%.1f deg)",
                         s_scan_index, kScanPoints[s_scan_index].name, kScanPoints[s_scan_index].yaw_deg);
                state.servo.target_yaw_deg.store(kScanPoints[s_scan_index].yaw_deg, std::memory_order_relaxed);
                state.set_balloon_text(kScanPoints[s_scan_index].msg, 2000);
                s_drive_state = AutoDriveState::ScanWait;
                s_state_start_ms = now_ms;
            } else {
                // 4方向完了、首を正面に戻して判定へ
                ESP_LOGI(kTag, "Scan complete for all 4 directions. Returning head to center.");
                state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                state.set_balloon_text("正面に戻して判定中…", 1500);
                s_drive_state = AutoDriveState::Decision;
                s_state_start_ms = now_ms;
            }
            break;
        }

        case AutoDriveState::Decision:
            // 首が正面に戻るのを待つ (800ms)
            if (now_ms - s_state_start_ms >= 800) {
                // 4方向の評価を集計
                int best_idx = -1;
                int best_score = -1;
                for (int i = 0; i < 4; ++i) {
                    if (s_scan_evals[i].success && s_scan_evals[i].passable && s_scan_evals[i].score > best_score) {
                        best_score = s_scan_evals[i].score;
                        best_idx = i;
                    }
                }

                if (best_idx < 0 || best_score < 40) {
                    // 全方向障害物（袋小路）：180度Uターン
                    ESP_LOGW(kTag, "All directions blocked (best score=%d). Executing 180 deg U-turn.", best_score);
                    state.set_balloon_text("行き止まりだ！Uターンするね！", 2500);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    s_target_turn_deg = 180.0f;
                    s_turn_spin_right = true;
                } else {
                    // 最善方向へ旋回
                    const auto& best_pt = kScanPoints[best_idx];
                    ESP_LOGI(kTag, "Best direction: %s (score=%d)", best_pt.name, best_score);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%sへ進路変更！（%d点）", best_pt.name, best_score);
                    state.set_balloon_text(buf, 2000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);

                    s_target_turn_deg = std::abs(best_pt.yaw_deg);
                    s_turn_spin_right = (best_pt.yaw_deg < 0); // 負が右、正が左
                }

                // IMU 旋回開始（クリーンな状態から角度積分を開始）
                s_turn_integrated_deg = 0.0f;
                s_turn_last_us = esp_timer_get_time();
                send_command(s_turn_spin_right ? CmdSpinRight : CmdSpinLeft);
                s_drive_state = AutoDriveState::Turning;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::Turning: {
            // IMU ジャイロ Z 角速度を積分
            const int64_t now_us = esp_timer_get_time();
            const float dt = (now_us - s_turn_last_us) / 1000000.0f;
            s_turn_last_us = now_us;

            float gx = 0, gy = 0, gz = 0;
            if (M5.Imu.getGyro(&gx, &gy, &gz)) {
                s_turn_integrated_deg += std::abs(gz) * dt;
            }

            // 目標角度到達判定 (または安全タイムアウト: 4秒)
            if (s_turn_integrated_deg >= s_target_turn_deg || (now_ms - s_state_start_ms >= 4000)) {
                send_command(CmdStop);
                ESP_LOGI(kTag, "Turn complete: integrated=%.1f deg (target=%.1f deg)",
                         s_turn_integrated_deg, s_target_turn_deg);
                state.set_balloon_text("向きを変えたよ！", 1500);
                s_drive_state = AutoDriveState::VerifyPath;
                s_state_start_ms = now_ms;
            }
            break;
        }

        case AutoDriveState::VerifyPath:
            // 旋回完了後、500ms 静止して正面の超音波距離を確認
            if (now_ms - s_state_start_ms >= 500) {
                if (dist_mm > 0 && dist_mm <= 120) {
                    // まだ前方に障害物がある場合は再探索
                    state.set_balloon_text("あれ？前が近いな…再確認！", 2000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                } else {
                    // 新進路クリア！前進再開
                    state.servo.speed_override.store(0, std::memory_order_relaxed);
                    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                    state.set_balloon_text("よし、クリア！進むよ！", 1500);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    send_command(CmdForward);
                    s_drive_state = AutoDriveState::Forward;
                    s_state_start_ms = now_ms;
                }
            }
            break;
        }
    }
}

} // namespace stackchan::app
