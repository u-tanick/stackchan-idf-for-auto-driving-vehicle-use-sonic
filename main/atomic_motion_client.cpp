// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"
#include "speech.hpp"

#include <cstdio>
#include <cmath>
#include "avatar/expression.hpp"
#include "vlm_client.hpp"
#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {
constexpr const char* kTag = "atomic_client";
constexpr uint32_t kI2cFreq = 100000;

static bool s_is_regular_driving_speech = false; // 通常走行中のランダム発話
static bool s_speech_pending = false;            // 発話タスク起動中〜再生中フラグ
static uint32_t s_speech_completed_ms = 0;       // 直近の発話終了時刻
static uint32_t s_next_drive_speech_ms = 0;      // 次回通常走行発話予定時刻

static const char32_t* kForwardDrivingPhrases[] = {
    U"ごー、ごーー",
    U"いけ、いけーーー",
    U"どんどん、すすむよーー",
    U"ここわ、どこだーーー",
};

void say_step(Speech& speech, SharedState& state, std::string_view display, std::u32string_view reading, uint32_t duration_ms = 2500) {
    state.set_balloon_text(display, duration_ms);
    if (!reading.empty()) {
        s_is_regular_driving_speech = false;
        s_speech_pending = true;
        speech.say(reading);
    }
}

bool is_speech_cooling_down(uint32_t now_ms, const Speech& speech) {
    if (s_is_regular_driving_speech) {
        return false;
    }
    if (speech.is_speaking() || s_speech_pending) {
        return true;
    }
    if (s_speech_completed_ms > 0 && (now_ms - s_speech_completed_ms < 1000)) {
        return true;
    }
    return false;
}

bool s_initialized = false;

SharedState::Driving::Mode s_last_synced_mode = SharedState::Driving::Mode::Autonomous;
bool s_mode_force_sync = true;
uint32_t s_last_tick_ms = 0;

enum class AutoDriveState {
    InitWait,         // 起動後の待機（サーボ初期診断完了待ち）
    Forward,          // 前進走行中
    ObstacleDetected, // 壁検知時の一時停止待機（1.5秒待機 & 発話終了後1秒待機）
    BackingUp,        // 10cm未満時の微速後退
    StartScan,        // 探索シーケンス開始（首振り開始）
    ScanWait,         // 首振り静止待ち（1秒）
    ScanEvaluate,     // 撮影 & VLM評価
    Decision,         // 全4方向の評価結果集計 & 最善方向決定
    Turning,          // IMUジャイロ積分による旋回中
    VerifyCamera,     // 旋回完了後、カメラで新進路を視認
    VerifySonic,      // カメラ確認後、超音波ONに戻して距離確認
    ErrorHold,        // 画像認識エラー等による停止・待機
};

struct ScanPoint {
    float yaw_deg;
    const char* name;
    const char* msg;
    const char32_t* reading;
};

static const ScanPoint kScanPoints[4] = {
    { -90.0f, "右(90度)", "右(90°)を見てみるね…", U"こっちわ、あいてるかな" },
    { -45.0f, "右斜め(45度)", "右斜め(45°)を見てみるね…", nullptr },
    { +90.0f, "左(90度)", "左(90°)を見てみるね…", U"はんたいわ、どうだろう" },
    { +45.0f, "左斜め(45度)", "左斜め(45°)を見てみるね…", nullptr }
};

AutoDriveState s_drive_state = AutoDriveState::InitWait;
uint32_t s_state_start_ms = 0;
int s_scan_index = 0;
int s_eval_retry_count = 0;
int s_verify_retry_count = 0;
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

void AtomicMotionClient::tick(SharedState& state, Speech& speech)
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

    // 発話ライフサイクル監視（再生中検知、終了タイミング記録、クールダウン管理）
    const bool speaking = speech.is_speaking();
    static bool s_was_speaking = false;
    if (speaking) {
        s_speech_pending = false; // 実際に再生中に入った
    }
    if (s_was_speaking && !speaking) {
        s_speech_completed_ms = now_ms;
        s_speech_pending = false;
        s_is_regular_driving_speech = false;
    }
    s_was_speaking = speaking;

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
        // ※超音波センサーをONに戻すタイミング: VerifySonic / Forward / InitWait のみ
        const bool ultrasonic_active = (s_drive_state == AutoDriveState::Forward ||
                                        s_drive_state == AutoDriveState::InitWait ||
                                        s_drive_state == AutoDriveState::VerifySonic);
        const bool is_ultrasonic_enabled = (current_mode != SharedState::Driving::Mode::Autonomous) || ultrasonic_active;

        static bool s_last_obstacle = false;
        static uint32_t s_last_balloon_update_ms = 0;

        const bool obstacle = (status.obstacle_flags & 0x01) != 0;
        const uint16_t dist_cm = status.distance_mm / 10;

        const uint16_t scan_dist_cm = state.driving.scan_distance_cm.load(std::memory_order_relaxed);
        const uint16_t scan_threshold_cm = (scan_dist_cm > 0 ? scan_dist_cm : 10);
        const uint16_t scan_threshold_mm = scan_threshold_cm * 10;

        const uint16_t alert_dist_cm = state.driving.alert_distance_cm.load(std::memory_order_relaxed);
        const uint16_t alert_threshold_cm = (alert_dist_cm > 0 ? alert_dist_cm : 5);
        const uint16_t alert_threshold_mm = alert_threshold_cm * 10;

        if (is_ultrasonic_enabled && s_drive_state != AutoDriveState::ErrorHold) {
            const uint16_t dist_mm = status.distance_mm;
            if (dist_mm > 0 && dist_mm <= alert_threshold_mm) {
                // 最接近アラート距離まで迫った場合：赤色、ネガティブ表情（Angry）
                state.face.bg_color.store(0xF800u, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Angry), std::memory_order_relaxed);
            } else if (dist_mm > alert_threshold_mm && dist_mm <= scan_threshold_mm) {
                // 接近検知距離まで迫った場合：青色、ネガティブ表情（Doubt）
                state.face.bg_color.store(0x001Fu, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
            } else if (dist_mm > scan_threshold_mm) {
                // 通常状態：標準の黒色、ポジティブ表情（Happy）
                state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
                if (s_drive_state == AutoDriveState::Forward || s_drive_state == AutoDriveState::InitWait) {
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                }
            }

            if (obstacle) {
                // 新規検知または検知継続中の定期更新（2秒ごと）
                if (!s_last_obstacle || (now_ms - s_last_balloon_update_ms >= 2000)) {
                    s_last_balloon_update_ms = now_ms;
                    char msg[64];
                    if (dist_mm > 0 && dist_mm <= alert_threshold_mm) {
                        snprintf(msg, sizeof(msg), "ぶつかるー！(%u cm)", dist_cm);
                    } else {
                        snprintf(msg, sizeof(msg), "かべ接近中！(%u cm)", dist_cm);
                    }
                    state.set_balloon_text(msg, 2000);
                }
            } else if (s_last_obstacle) {
                // 障害物がなくなった時
                state.set_balloon_text("よし、クリア！", 1500);
            }
        }
        s_last_obstacle = obstacle;
    }

    // If Autonomous, run VLM & IMU based obstacle avoidance state machine
    if (current_mode == SharedState::Driving::Mode::Autonomous) {
        const uint16_t dist_mm = state.driving.distance_mm.load(std::memory_order_relaxed);
        const uint16_t scan_dist_cm = state.driving.scan_distance_cm.load(std::memory_order_relaxed);
        const uint16_t scan_threshold_cm = (scan_dist_cm > 0 ? scan_dist_cm : 10);
        const uint16_t scan_threshold_mm = scan_threshold_cm * 10;

        switch (s_drive_state) {
        case AutoDriveState::InitWait:
            // 起動後 15 秒（サーボセルフテスト完了）待機してから前進開始
            if (now_ms >= 15000 && dist_mm > 0) {
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                say_step(speech, state, "前進スタート！", U"ぜんしん、すたーと", 1500);
                send_command(CmdForward);
                s_drive_state = AutoDriveState::Forward;
                s_state_start_ms = now_ms;
                s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
            }
            break;

        case AutoDriveState::Forward:
            send_command(CmdForward);
            state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);

            // 通常前進走行中のランダム定期発話（4〜8秒間隔、4種からランダム選択、吹き出しなし）
            if (now_ms >= s_next_drive_speech_ms && !speech.is_speaking() && !s_speech_pending) {
                s_is_regular_driving_speech = true;
                s_speech_pending = true;
                const size_t phrase_idx = esp_random() % (sizeof(kForwardDrivingPhrases) / sizeof(kForwardDrivingPhrases[0]));
                speech.say(kForwardDrivingPhrases[phrase_idx]);
                s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
            }

            // 接近検知距離以内で壁を検知したら停止
            if (dist_mm > 0 && dist_mm <= scan_threshold_mm) {
                send_command(CmdStop);
                // 通常前進発話があれば即座に割り込みキャンセル
                speech.stop();
                s_is_regular_driving_speech = false;
                s_speech_pending = false;

                char buf[32];
                std::snprintf(buf, sizeof(buf), "かべ検知！（%u cm）", dist_mm / 10);
                state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                state.face.bg_color.store(0x001Fu, std::memory_order_relaxed); // 青色
                say_step(speech, state, buf, U"いきどまりかな", 2000);

                s_drive_state = AutoDriveState::ObstacleDetected;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::ObstacleDetected: {
            send_command(CmdStop);
            // 接近検知から最低1.5秒経過、かつ「いきどまりかな」発話終了後1秒経過を待つ
            const bool delay_ok = (now_ms - s_state_start_ms >= 1500);
            const bool speech_ok = !is_speech_cooling_down(now_ms, speech);
            if (delay_ok && speech_ok) {
                const uint16_t back_margin_mm = (scan_threshold_mm > 20) ? (scan_threshold_mm - 10) : scan_threshold_mm;
                if (dist_mm > 0 && dist_mm < back_margin_mm) {
                    // 目標距離未満まで近接してしまったら後退へ
                    s_drive_state = AutoDriveState::BackingUp;
                    s_state_start_ms = now_ms;
                } else {
                    // 目標距離付近で停止、首振り探索へ
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                }
            }
            break;
        }

        case AutoDriveState::BackingUp:
            // 目標距離に達するまで微速後退
            send_command(CmdBackward);
            state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
            if (dist_mm >= scan_threshold_mm || (now_ms - s_state_start_ms >= 1500)) {
                send_command(CmdStop);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%u cmまで下がったよ", scan_threshold_cm);
                state.set_balloon_text(buf, 1500);
                s_drive_state = AutoDriveState::StartScan;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::StartScan:
            // 首振り開始: 最初の方向 (右90°) へ向ける
            s_scan_index = 0;
            ESP_LOGI(kTag, "Start scan sequence. Target 0: %s (yaw=%.1f deg)",
                     kScanPoints[0].name, kScanPoints[0].yaw_deg);
            state.servo.speed_override.store(250, std::memory_order_relaxed);
            state.servo.target_yaw_deg.store(kScanPoints[0].yaw_deg, std::memory_order_relaxed);
            state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
            state.face.bg_color.store(0x001Fu, std::memory_order_relaxed);
            if (kScanPoints[0].reading != nullptr) {
                say_step(speech, state, kScanPoints[0].msg, kScanPoints[0].reading, 2500);
            } else {
                state.set_balloon_text(kScanPoints[0].msg, 2000);
            }
            s_drive_state = AutoDriveState::ScanWait;
            s_state_start_ms = now_ms;
            break;

        case AutoDriveState::ScanWait:
            // 首振り後、手ブレ防止のため 1 秒静止。さらに発話中・発話後クールダウン中は待機
            if (now_ms - s_state_start_ms >= 1000 && !is_speech_cooling_down(now_ms, speech)) {
                s_drive_state = AutoDriveState::ScanEvaluate;
            }
            break;

        case AutoDriveState::ScanEvaluate: {
            // 現在のカメラフレームを VLM で評価
            const auto& pt = kScanPoints[s_scan_index];
            ESP_LOGI(kTag, "Evaluating view for %s (try %d/3)...", pt.name, s_eval_retry_count + 1);
            VlmEvaluation eval = VlmClient::evaluate_current_view(pt.name);

            if (!eval.success) {
                s_eval_retry_count++;
                ESP_LOGW(kTag, "VLM eval error at %s (retry %d/3)", pt.name, s_eval_retry_count);
                if (s_eval_retry_count < 3) {
                    state.face.expression.store(static_cast<int>(avatar::Expression::Sad), std::memory_order_relaxed);
                    char retry_msg[32];
                    std::snprintf(retry_msg, sizeof(retry_msg), "再試行中(%d/3)", s_eval_retry_count);
                    state.set_balloon_text(retry_msg, 1500);
                    // 1秒待って同じ角度で再評価
                    s_drive_state = AutoDriveState::ScanWait;
                    s_state_start_ms = now_ms;
                    break;
                } else {
                    // 3回すべて失敗
                    ESP_LOGE(kTag, "VLM eval failed 3 times at %s. Triggering help notification.", pt.name);
                    s_eval_retry_count = 0;
                    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                    state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // 接続失敗：赤色
                    state.face.expression.store(static_cast<int>(avatar::Expression::Sad), std::memory_order_relaxed);
                    say_step(speech, state, "画像認識失敗", U"たすけてー、にんしき、しっぱい", 6000);
                    send_command(CmdStop);
                    s_drive_state = AutoDriveState::ErrorHold;
                    s_state_start_ms = now_ms;
                    break;
                }
            }

            s_eval_retry_count = 0; // 成功したためリセット
            s_scan_evals[s_scan_index] = eval;

            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s: %d点 (%s)", pt.name, eval.score, eval.passable ? "OK" : "NG");
            state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
            state.set_balloon_text(buf, 2000);

            s_scan_index++;
            if (s_scan_index < 4) {
                // 次の方向へ首を向ける
                ESP_LOGI(kTag, "Next scan target %d: %s (yaw=%.1f deg)",
                         s_scan_index, kScanPoints[s_scan_index].name, kScanPoints[s_scan_index].yaw_deg);
                state.servo.target_yaw_deg.store(kScanPoints[s_scan_index].yaw_deg, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                if (kScanPoints[s_scan_index].reading != nullptr) {
                    say_step(speech, state, kScanPoints[s_scan_index].msg, kScanPoints[s_scan_index].reading, 2500);
                } else {
                    state.set_balloon_text(kScanPoints[s_scan_index].msg, 2000);
                }
                s_drive_state = AutoDriveState::ScanWait;
                s_state_start_ms = now_ms;
            } else {
                // 4方向完了、首を正面に戻して判定へ
                ESP_LOGI(kTag, "Scan complete for all 4 directions. Returning head to center.");
                state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                state.set_balloon_text("判定中…", 1500);
                s_drive_state = AutoDriveState::Decision;
                s_state_start_ms = now_ms;
            }
            break;
        }

        case AutoDriveState::Decision:
            // 首が正面に戻るのを待つ (800ms) かつ 発話終了待機
            if (now_ms - s_state_start_ms >= 800 && !is_speech_cooling_down(now_ms, speech)) {
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
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    say_step(speech, state, "行き止まり！", U"ゆきどまりだ、ゆーたーんするよ", 2500);
                    s_target_turn_deg = 180.0f;
                    s_turn_spin_right = true;
                } else {
                    // 最善方向へ進路決定
                    const auto& best_pt = kScanPoints[best_idx];
                    ESP_LOGI(kTag, "Best direction: %s (score=%d)", best_pt.name, best_score);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%sへ進路変更！", best_pt.name);
                    state.set_balloon_text(buf, 2000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);

                    s_target_turn_deg = std::abs(best_pt.yaw_deg);
                    s_turn_spin_right = (best_pt.yaw_deg < 0); // 負が右、正が左
                }

                // 机上テスト対応: 車体が自力旋回できない場合でも進路方向を視認できるよう首を進路に向ける
                state.servo.target_yaw_deg.store(best_idx >= 0 ? kScanPoints[best_idx].yaw_deg : 0.0f, std::memory_order_relaxed);

                // 旋回開始（机上テスト時は1.2秒の旋回演出後にカメラ視認へ進行）
                s_turn_integrated_deg = 0.0f;
                s_turn_last_us = esp_timer_get_time();
                state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                say_step(speech, state, "方向転換するよ", U"しんこうほうこう、へんこう", 2000);
                send_command(s_turn_spin_right ? CmdSpinRight : CmdSpinLeft);
                s_drive_state = AutoDriveState::Turning;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::Turning: {
            // 机上テスト対応（DCモーター未接続時）:
            // 手動旋回によるジャイロ積分、または1.2秒の旋回演出完了で次ステップへ進む
            const int64_t now_us = esp_timer_get_time();
            const float dt = (now_us - s_turn_last_us) / 1000000.0f;
            s_turn_last_us = now_us;

            float gx = 0, gy = 0, gz = 0;
            if (M5.Imu.getGyro(&gx, &gy, &gz)) {
                s_turn_integrated_deg += std::abs(gz) * dt;
            }

            // 目標角度到達（手動旋回含む）または机上旋回シミュレーション時間（1200ms）経過
            if (s_turn_integrated_deg >= s_target_turn_deg || (now_ms - s_state_start_ms >= 1200)) {
                send_command(CmdStop);
                ESP_LOGI(kTag, "Turn complete: integrated=%.1f deg (target=%.1f deg). Proceeding to camera visual check.",
                         s_turn_integrated_deg, s_target_turn_deg);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                say_step(speech, state, "カメラで確認中", U"じゃまなものわ、ないかな", 2500);
                s_drive_state = AutoDriveState::VerifyCamera;
                s_state_start_ms = now_ms;
            }
            break;
        }

        case AutoDriveState::VerifyCamera:
            // 旋回直後の手ブレ静止待ち (800ms) かつ 発話終了後1秒待機
            if (now_ms - s_state_start_ms >= 800 && !is_speech_cooling_down(now_ms, speech)) {
                ESP_LOGI(kTag, "Verifying new path with camera view (front_check, try %d/3)...", s_verify_retry_count + 1);
                VlmEvaluation eval = VlmClient::evaluate_current_view("front_check");
                if (!eval.success) {
                    s_verify_retry_count++;
                    ESP_LOGW(kTag, "Front check VLM failed (retry %d/3)", s_verify_retry_count);
                    if (s_verify_retry_count < 3) {
                        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                        state.set_balloon_text("正面確認 再試行中…", 1500);
                        s_state_start_ms = now_ms;
                        break;
                    } else {
                        s_verify_retry_count = 0;
                        state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                        state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // 接続失敗：赤色
                        state.face.expression.store(static_cast<int>(avatar::Expression::Sad), std::memory_order_relaxed);
                        say_step(speech, state, "画像認識失敗", U"たすけてー、にんしき、しっぱい", 6000);
                        send_command(CmdStop);
                        s_drive_state = AutoDriveState::ErrorHold;
                        s_state_start_ms = now_ms;
                        break;
                    }
                }
                s_verify_retry_count = 0;

                if (!eval.passable || eval.score < 40) {
                    ESP_LOGW(kTag, "Camera detected obstacle in new path (score=%d). Rescanning...", eval.score);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    say_step(speech, state, "障害物あり！", U"しょうがいぶつ、はっけん", 2500);
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                } else {
                    ESP_LOGI(kTag, "Camera path clear (score=%d). Now verifying with ultrasonic sensor...", eval.score);
                    // カメラ確認OK → 首を正面に戻して超音波センサーをONにし距離確認
                    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                    state.set_balloon_text("センサー確認中", 2000); // 発話なし
                    s_drive_state = AutoDriveState::VerifySonic;
                    s_state_start_ms = now_ms;
                }
            }
            break;

        case AutoDriveState::VerifySonic:
            // 超音波センサーの測定値反映待ち (600ms)
            if (now_ms - s_state_start_ms >= 600) {
                const uint16_t current_dist = state.driving.distance_mm.load(std::memory_order_relaxed);
                const uint16_t verify_limit_mm = scan_threshold_mm + 20;
                if (current_dist > 0 && current_dist <= verify_limit_mm) {
                    ESP_LOGW(kTag, "Ultrasonic sensor detected obstacle (%u mm). Rescanning...", current_dist);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    say_step(speech, state, "壁検知！再探索", U"しょうがいぶつ、はっけん", 2500);
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                } else {
                    // カメラ・超音波ともにOK！
                    ESP_LOGI(kTag, "Both camera and ultrasonic clear! Resuming forward drive.");
                    state.servo.speed_override.store(0, std::memory_order_relaxed);
                    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                    state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 標準黒
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    say_step(speech, state, "よし！クリア", U"もんだいなし、れっつごー", 1500);
                    send_command(CmdForward);
                    s_drive_state = AutoDriveState::Forward;
                    s_state_start_ms = now_ms;
                    s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
                }
            }
            break;

        case AutoDriveState::ErrorHold:
            send_command(CmdStop);
            state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
            state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // エラー停止中は赤色を維持
            // 画面タップがあれば再探索シーケンスへ復帰
            if (M5.Touch.getCount() > 0) {
                ESP_LOGI(kTag, "Screen tapped in ErrorHold. Resuming scan sequence...");
                state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 標準黒
                state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                say_step(speech, state, "再探索するよ", U"もういちど、さがすよ", 1500);
                s_drive_state = AutoDriveState::StartScan;
                s_state_start_ms = now_ms;
            }
            break;
        }
    }
}

} // namespace stackchan::app
