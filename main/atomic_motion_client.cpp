// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"
#include "mode_select_screen.hpp"
#include "speech.hpp"
#include "wifi_sta.hpp"

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
static bool s_init_check_failed = false;         // 起動時ヘルスチェック失敗フラグ

// 音声合成タスクがビジーな場合にドロップせず保持するキュー
static std::u32string s_pending_speech_reading;
static bool s_has_pending_speech = false;

struct DrivingPhrase {
    const char* display;
    const char32_t* reading;
};

static const DrivingPhrase kForwardDrivingPhrases[] = {
    { "ごー、ごーー", U"ごー、ごーー" },
    { "いけ、いけーーー", U"いけ、いけーーー" },
    { "どんどん、すすむよーー", U"どんどん、すすむよーー" },
    { "ここは、どこだーーー", U"ここわ、どこだーーー" },
};

void say_step(Speech& speech, SharedState& state, std::string_view display, std::u32string_view reading, uint32_t duration_ms = 2500) {
    state.set_balloon_text(display, duration_ms);
    if (!reading.empty()) {
        s_is_regular_driving_speech = false;
        if (speech.say(reading)) {
            s_speech_pending = true;
            s_has_pending_speech = false;
            s_pending_speech_reading.clear();
        } else {
            // 前の合成タスクが完了するまでキューに保持して次tickで即時実行
            s_has_pending_speech = true;
            s_pending_speech_reading = std::u32string(reading);
            s_speech_pending = true;
            ESP_LOGI(kTag, "Speech busy, queued reading: %.*s", static_cast<int>(display.size()), display.data());
        }
    }
}

bool is_speech_cooling_down(uint32_t now_ms, const Speech& speech) {
    if (s_is_regular_driving_speech) {
        return false;
    }
    if (s_has_pending_speech || speech.is_speaking() || s_speech_pending) {
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
    Standby,          // 停止待機中（画面タップでスタート）
    StartWait,        // 「スタートするよ」発話待機（1.2秒後に前進開始）
    CameraCheck,      // 距離＋カメラモード起動時の事前ヘルスチェック（Wi-Fi & VLM確認）
    Forward,          // 前進走行中
    ObstacleDetected, // 壁検知時の一時停止・「いきどまりかな」発話待機
    ObstacleDelay,    // 「いきどまりかな」発話完了後のディレイ待機（1秒）
    BackingUp,        // 10cm未満時の微速後退
    StartScan,        // 探索シーケンス開始（首振り開始）
    ScanHeadMoving,   // 顔の向き変更中待機（800ms）
    ScanWait,         // 首振り完了後の発話終了＆手ブレ防止静止待ち（1秒）
    ScanEvaluate,     // 撮影 & VLM評価
    Decision,         // 全4方向の評価結果集計 & 最善方向決定
    Turning,          // IMUジャイロ積分による旋回中
    VerifySonicOnly,  // 距離センサーモード: 旋回後の前方距離確認（クリアなら前進、壁なら再度旋回）
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
bool s_obstacle_speech_active = false;
bool s_start_speech_active = false;
uint32_t s_start_delay_ms = 0;
uint32_t s_last_toggle_ms = 0;
AtomicMotionClient::DriveType s_drive_type = AtomicMotionClient::DriveType::SonicOnly;
bool s_mode_selected = false;
bool s_drive_type_speech_pending = false;
uint32_t s_mode_selected_ms = 0;
bool s_standby_prompt_shown = false;
bool s_sonic_retried = false;

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

bool AtomicMotionClient::is_connected()
{
    if (!s_initialized) return false;
    return M5.Ex_I2C.scanID(kDefaultSlaveAddr, kI2cFreq);
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

    // Read Robot Mode (0x04)
    uint8_t mode_byte = 0;
    if (M5.Ex_I2C.start(kDefaultSlaveAddr, false, kI2cFreq) &&
        M5.Ex_I2C.write(0x04) &&
        M5.Ex_I2C.stop() &&
        M5.Ex_I2C.start(kDefaultSlaveAddr, true, kI2cFreq) &&
        M5.Ex_I2C.read(&mode_byte, 1) &&
        M5.Ex_I2C.stop()) {
        out_status.robot_mode = mode_byte;
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

    // 未送信の pending 発話があれば、合成タスクが空き次第即座に開始！
    if (s_has_pending_speech) {
        if (speech.say(s_pending_speech_reading)) {
            ESP_LOGI(kTag, "Queued speech successfully started!");
            s_has_pending_speech = false;
            s_pending_speech_reading.clear();
            s_speech_pending = true;
        }
    }

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

        // AtomS3 Lite の実モードが CoreS3 の期待とズレていたら自動で再同期
        const uint8_t expected_mode_val = (current_mode == SharedState::Driving::Mode::Manual) ? 0x01 : 0x00;
        if (status.robot_mode != expected_mode_val) {
            static uint32_t s_last_mode_warn_ms = 0;
            if (now_ms - s_last_mode_warn_ms >= 1000) {
                s_last_mode_warn_ms = now_ms;
                ESP_LOGW(kTag, "Mode mismatch! CoreS3=%s, AtomS3=%s. Resynchronizing...",
                         expected_mode_val ? "Manual" : "Autonomous",
                         status.robot_mode ? "Manual" : "Autonomous");
            }
            set_mode(current_mode);
        }

        static uint32_t s_last_telemetry_log_ms = 0;
        if (now_ms - s_last_telemetry_log_ms >= 1000) {
            s_last_telemetry_log_ms = now_ms;
            const char* state_str = "Unknown";
            switch (s_drive_state) {
                case AutoDriveState::InitWait: state_str = "InitWait"; break;
                case AutoDriveState::Standby: state_str = "Standby"; break;
                case AutoDriveState::StartWait: state_str = "StartWait"; break;
                case AutoDriveState::CameraCheck: state_str = "CameraCheck"; break;
                case AutoDriveState::Forward: state_str = "Forward"; break;
                case AutoDriveState::ObstacleDetected: state_str = "ObstacleDetected"; break;
                case AutoDriveState::ObstacleDelay: state_str = "ObstacleDelay"; break;
                case AutoDriveState::BackingUp: state_str = "BackingUp"; break;
                case AutoDriveState::StartScan: state_str = "StartScan"; break;
                case AutoDriveState::ScanHeadMoving: state_str = "ScanHeadMoving"; break;
                case AutoDriveState::ScanWait: state_str = "ScanWait"; break;
                case AutoDriveState::ScanEvaluate: state_str = "ScanEvaluate"; break;
                case AutoDriveState::Decision: state_str = "Decision"; break;
                case AutoDriveState::Turning: state_str = "Turning"; break;
                case AutoDriveState::VerifySonicOnly: state_str = "VerifySonicOnly"; break;
                case AutoDriveState::VerifyCamera: state_str = "VerifyCamera"; break;
                case AutoDriveState::VerifySonic: state_str = "VerifySonic"; break;
                case AutoDriveState::ErrorHold: state_str = "ErrorHold"; break;
            }
            ESP_LOGI(kTag, "Telemetry: dist=%u mm, obs=0x%02X, state=%s, mode=%s (atom=%s, selected=%d, joy_active=%d, is_moving=%d)",
                     status.distance_mm, status.obstacle_flags, state_str,
                     (current_mode == SharedState::Driving::Mode::Manual) ? "Manual" : "Auto",
                     (status.robot_mode == 1) ? "Manual" : "Auto", s_mode_selected,
                     status.joy_active ? 1 : 0,
                     state.driving.is_moving.load(std::memory_order_relaxed) ? 1 : 0);
        }

        // 手動操縦モード（JoyC）のときの画面案内表示および走行中発話
        // モード選択直後（3.5秒間）は「JoyC操作モード」の吹き出しを維持するため上書きしない
        if (current_mode == SharedState::Driving::Mode::Manual && (now_ms - s_mode_selected_ms >= 3500)) {
            static int s_last_joy_active_state = -1;
            const int joy_now = status.joy_active ? 1 : 0;
            if (joy_now != s_last_joy_active_state) {
                s_last_joy_active_state = joy_now;
                if (joy_now) {
                    state.set_balloon_text("JoyC操縦中 (受信中)", 3000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                } else {
                    state.set_balloon_text("JoyC待機中 (TX OFF)", 3000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                }
            }

            // JoyC走行中（通信中）のランダム定期発話（4〜8秒間隔、4種からランダム選択、吹き出し表示）
            if (joy_now && now_ms >= s_next_drive_speech_ms && !speech.is_speaking() && !s_speech_pending) {
                s_is_regular_driving_speech = true;
                s_speech_pending = true;
                const size_t phrase_idx = esp_random() % (sizeof(kForwardDrivingPhrases) / sizeof(kForwardDrivingPhrases[0]));
                const auto& phrase = kForwardDrivingPhrases[phrase_idx];
                state.set_balloon_text(phrase.display, 2000);
                speech.say(phrase.reading);
                s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
            }
        }

        // 障害物検知時の画面演出（フキダシ・セリフ・表情フィードバック）
        // ※超音波センサーによる演出は、自律走行モード(Autonomous)の走行中のみ有効（JoyC手動モードでは使用しない）
        const bool ultrasonic_active = (s_drive_state == AutoDriveState::Forward ||
                                        s_drive_state == AutoDriveState::BackingUp ||
                                        s_drive_state == AutoDriveState::VerifySonicOnly ||
                                        s_drive_state == AutoDriveState::VerifySonic);
        const bool is_ultrasonic_enabled = (current_mode == SharedState::Driving::Mode::Autonomous) && ultrasonic_active;

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
            s_last_obstacle = obstacle;
        } else {
            s_last_obstacle = false;
        }
    }

    // モード選択時の案内発話保留があれば再生
    if (s_drive_type_speech_pending && !speech.is_speaking()) {
        s_drive_type_speech_pending = false;
        if (s_drive_type == DriveType::JoyCManual) {
            say_step(speech, state, "JoyC操作モード", U"じょいしー、そうさもーど", 3500);
        } else if (s_drive_type == DriveType::SonicOnly) {
            say_step(speech, state, "距離センサーモード", U"きょりせんさー、もーど", 3500);
        } else if (s_drive_type == DriveType::SonicCamera) {
            say_step(speech, state, "カメラ確認中…", U"きょりと、かめらもーど、ちぇっくちゅう", 3000);
        }
    }

    // 自律運転待機時、モード選択後に「タップでスタート！」の案内吹き出しを表示（タップされるまで待機）
    if (s_mode_selected && current_mode == SharedState::Driving::Mode::Autonomous && s_drive_state == AutoDriveState::Standby) {
        if (!s_standby_prompt_shown && !speech.is_speaking()) {
            s_standby_prompt_shown = true;
            state.set_balloon_text("タップでスタート！", 5000);
        }
    }

    // If Autonomous, run VLM & IMU based obstacle avoidance state machine
    if (current_mode == SharedState::Driving::Mode::Autonomous) {
        const uint16_t dist_mm = state.driving.distance_mm.load(std::memory_order_relaxed);
        const uint16_t scan_dist_cm = state.driving.scan_distance_cm.load(std::memory_order_relaxed);
        const uint16_t scan_threshold_cm = (scan_dist_cm > 0 ? scan_dist_cm : 10);
        const uint16_t scan_threshold_mm = scan_threshold_cm * 10;

        switch (s_drive_state) {
        case AutoDriveState::InitWait:
            // 起動後 15 秒（サーボセルフテスト完了）待機してからモード選択画面を表示 (Atom接続時のみ)
            if (now_ms >= 15000 && is_connected()) {
                send_command(CmdStop);
                state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
                ESP_LOGI(kTag, "Servo self-test complete & Atom connected. Showing mode select screen.");
                mode_select::show();
                s_drive_state = AutoDriveState::Standby;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::Standby:
            // 停止待機中: モーター停止維持。画面タップでStartWaitへ移行する
            send_command(CmdStop);
            break;

        case AutoDriveState::StartWait: {
            send_command(CmdStop);
            const bool is_speaking = speech.is_speaking() || s_has_pending_speech || s_speech_pending;
            if (is_speaking) {
                s_start_speech_active = true;
            }
            // 発話が開始された後に終了したか、または安全タイムアウト(4.5秒)経過
            const bool finished = s_start_speech_active && !is_speaking;
            const bool timeout = (now_ms - s_state_start_ms >= 4500);

            if (finished || timeout) {
                // 発話完了後、自然な間（350ms）を置いてから前進開始（タップ→発話→走り出しのテンポ）
                if (s_start_delay_ms == 0) {
                    s_start_delay_ms = now_ms;
                }
                if (now_ms - s_start_delay_ms >= 350) {
                    ESP_LOGI(kTag, "Start speech finished (active=%d, timeout=%d). Moving forward!",
                             s_start_speech_active, timeout);
                    send_command(CmdForward);
                    s_drive_state = AutoDriveState::Forward;
                    s_state_start_ms = now_ms;
                    s_start_speech_active = false;
                    s_start_delay_ms = 0;
                    s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
                }
            }
            break;
        }

        case AutoDriveState::CameraCheck: {
            send_command(CmdStop);
            state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);

            // モード案内発話中であれば待機
            if (speech.is_speaking() || s_has_pending_speech || s_speech_pending || is_speech_cooling_down(now_ms, speech)) {
                break;
            }

            // 1. Wi-Fi 接続確認
            if (!wifi_is_connected()) {
                ESP_LOGE(kTag, "CameraCheck: Wi-Fi not connected!");
                s_init_check_failed = true;
                state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // 赤色背景
                state.face.expression.store(static_cast<int>(avatar::Expression::Angry), std::memory_order_relaxed);
                say_step(speech, state, "Wi-Fi未接続！", U"わいふぁいに、つながっていません", 4000);
                s_drive_state = AutoDriveState::ErrorHold;
                s_state_start_ms = now_ms;
                break;
            }

            // 2. カメラ画像取得 ＆ VLM 疎通テスト
            ESP_LOGI(kTag, "CameraCheck: Wi-Fi OK. Testing VLM connectivity with camera capture...");
            state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
            state.set_balloon_text("VLM接続テスト中…", 3000);

            VlmEvaluation eval = VlmClient::evaluate_current_view("health_check");
            if (!eval.success) {
                ESP_LOGE(kTag, "CameraCheck: VLM eval failed! reason: %s", eval.reason.c_str());
                s_init_check_failed = true;
                state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // 赤色背景
                state.face.expression.store(static_cast<int>(avatar::Expression::Angry), std::memory_order_relaxed);
                say_step(speech, state, "VLM接続失敗！", U"ぶいえるえむと、つながりません", 4000);
                s_drive_state = AutoDriveState::ErrorHold;
                s_state_start_ms = now_ms;
                break;
            }

            // 3. Wi-Fi & VLM どちらも成功！
            ESP_LOGI(kTag, "CameraCheck: All passed! (passable=%d, score=%d). Entering Standby.", eval.passable, eval.score);
            state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 通常黒色
            state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
            say_step(speech, state, "カメラ確認完了！", U"かめら、おっけー、れっつごー", 2000);

            s_drive_state = AutoDriveState::Standby;
            s_mode_selected_ms = now_ms; // 「タップでスタート！」カウントダウン開始
            s_standby_prompt_shown = false;
            s_state_start_ms = now_ms;
            break;
        }

        case AutoDriveState::Forward:
            send_command(CmdForward);
            state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);

            // 通常前進走行中のランダム定期発話（4〜8秒間隔、4種からランダム選択、吹き出し表示）
            if (now_ms >= s_next_drive_speech_ms && !speech.is_speaking() && !s_speech_pending) {
                s_is_regular_driving_speech = true;
                s_speech_pending = true;
                const size_t phrase_idx = esp_random() % (sizeof(kForwardDrivingPhrases) / sizeof(kForwardDrivingPhrases[0]));
                const auto& phrase = kForwardDrivingPhrases[phrase_idx];
                state.set_balloon_text(phrase.display, 2000);
                speech.say(phrase.reading);
                s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
            }

            // 接近検知距離以内で壁を検知したら停止
            if (dist_mm > 0 && dist_mm <= scan_threshold_mm) {
                send_command(CmdStop);
                // 通常前進発話があれば即座に割り込みキャンセル
                speech.stop();
                s_is_regular_driving_speech = false;
                s_has_pending_speech = false;
                s_pending_speech_reading.clear();

                char buf[32];
                std::snprintf(buf, sizeof(buf), "かべ検知！（%u cm）", dist_mm / 10);
                state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                state.face.bg_color.store(0x001Fu, std::memory_order_relaxed); // 青色
                ESP_LOGI(kTag, "WALL DETECTED: dist=%u mm (threshold=%u mm). Triggering speech: いきどまりかな",
                         dist_mm, scan_threshold_mm);
                say_step(speech, state, buf, U"いきどまりかな", 2000);

                s_obstacle_speech_active = false;
                s_drive_state = AutoDriveState::ObstacleDetected;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::ObstacleDetected: {
            send_command(CmdStop);
            const bool is_speaking = speech.is_speaking() || s_has_pending_speech || s_speech_pending;
            if (is_speaking) {
                s_obstacle_speech_active = true; // 発話または合成が開始された
            }
            // 発話が開始された後に終了したか、または安全タイムアウト(3.5秒)経過
            const bool finished = s_obstacle_speech_active && !is_speaking;
            const bool timeout = (now_ms - s_state_start_ms >= 3500);

            if (finished || timeout) {
                ESP_LOGI(kTag, "Obstacle speech finished (active=%d, timeout=%d). Entering ObstacleDelay (1000ms).",
                         s_obstacle_speech_active, timeout);
                s_drive_state = AutoDriveState::ObstacleDelay;
                s_state_start_ms = now_ms;
            }
            break;
        }

        case AutoDriveState::ObstacleDelay: {
            send_command(CmdStop);
            // 発話終了後、ユーザー希望のディレイ（1.0秒間静止待機）
            if (now_ms - s_state_start_ms >= 1000) {
                s_sonic_retried = false;
                // 壁との距離が10cm (100mm) 以下の場合は後退ステートへ
                if (dist_mm > 0 && dist_mm < 100) {
                    ESP_LOGI(kTag, "Distance (%u mm) < 100 mm. Backing up first.", dist_mm);
                    state.set_balloon_text("少し下がるね", 1500);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    send_command(CmdBackward);
                    s_drive_state = AutoDriveState::BackingUp;
                    s_state_start_ms = now_ms;
                } else {
                    // すでに10cm以上離れていれば直接旋回（または探索）へ
                    if (s_drive_type == DriveType::SonicOnly) {
                        ESP_LOGI(kTag, "Distance ok (%u mm). Turning left 90 deg.", dist_mm);
                        s_target_turn_deg = 90.0f;
                        s_turn_spin_right = false; // 左回転
                        s_turn_integrated_deg = 0.0f;
                        s_turn_last_us = esp_timer_get_time();
                        send_command(CmdSpinLeft);
                        state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                        state.set_balloon_text("左へ方向転換！", 1500);
                        s_drive_state = AutoDriveState::Turning;
                        s_state_start_ms = now_ms;
                    } else {
                        ESP_LOGI(kTag, "Obstacle delay complete. Transitioning to StartScan (head moving).");
                        s_drive_state = AutoDriveState::StartScan;
                        s_state_start_ms = now_ms;
                    }
                }
            }
            break;
        }

        case AutoDriveState::BackingUp:
            // 10cm (100mm) 以上離れるまで微速後退
            send_command(CmdBackward);
            state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
            if (dist_mm >= 100 || (now_ms - s_state_start_ms >= 2500)) {
                send_command(CmdStop);
                state.set_balloon_text("よし、ここまで下がったよ", 1500);
                if (s_drive_type == DriveType::SonicOnly) {
                    ESP_LOGI(kTag, "Backing up complete (%u mm). Turning left 90 deg.", dist_mm);
                    s_target_turn_deg = 90.0f;
                    s_turn_spin_right = false; // 左回転
                    s_turn_integrated_deg = 0.0f;
                    s_turn_last_us = esp_timer_get_time();
                    send_command(CmdSpinLeft);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    state.set_balloon_text("左へ方向転換！", 1500);
                    s_drive_state = AutoDriveState::Turning;
                    s_state_start_ms = now_ms;
                } else {
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                }
            }
            break;

        case AutoDriveState::StartScan:
            // 首振り開始: 最初の方向 (右90°) へ向ける（※まだ喋らない！）
            s_scan_index = 0;
            ESP_LOGI(kTag, "Start scan sequence. Target 0: %s (yaw=%.1f deg) - rotating head first",
                     kScanPoints[0].name, kScanPoints[0].yaw_deg);
            state.servo.speed_override.store(250, std::memory_order_relaxed);
            state.servo.target_yaw_deg.store(kScanPoints[0].yaw_deg, std::memory_order_relaxed);
            state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
            state.face.bg_color.store(0x001Fu, std::memory_order_relaxed);
            state.set_balloon_text(kScanPoints[0].msg, 2500);

            // 顔の向き変更完了を待つステートへ遷移
            s_drive_state = AutoDriveState::ScanHeadMoving;
            s_state_start_ms = now_ms;
            break;

        case AutoDriveState::ScanHeadMoving:
            // 顔の向き変更（サーボ回転）完了を待機 (800ms)
            if (now_ms - s_state_start_ms >= 800) {
                // 首が向き終わったら、ここでセリフを発話！
                const auto& pt = kScanPoints[s_scan_index];
                if (pt.reading != nullptr) {
                    ESP_LOGI(kTag, "Head aligned for %s. Triggering speech.", pt.name);
                    say_step(speech, state, pt.msg, pt.reading, 2500);
                } else {
                    state.set_balloon_text(pt.msg, 2000);
                }
                s_drive_state = AutoDriveState::ScanWait;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::ScanWait: {
            // 首振り完了後の発話終了、および手ブレ防止のため 1 秒静止
            const bool is_speaking = speech.is_speaking() || s_has_pending_speech || s_speech_pending;
            if (now_ms - s_state_start_ms >= 1000 && !is_speaking && !is_speech_cooling_down(now_ms, speech)) {
                s_drive_state = AutoDriveState::ScanEvaluate;
            }
            break;
        }

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
                // 次の方向へ首を向ける（※まだ喋らない！）
                ESP_LOGI(kTag, "Next scan target %d: %s (yaw=%.1f deg) - rotating head first",
                         s_scan_index, kScanPoints[s_scan_index].name, kScanPoints[s_scan_index].yaw_deg);
                state.servo.target_yaw_deg.store(kScanPoints[s_scan_index].yaw_deg, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                state.set_balloon_text(kScanPoints[s_scan_index].msg, 2500);

                // 首の回転待ちステートへ遷移
                s_drive_state = AutoDriveState::ScanHeadMoving;
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
                    if (s_scan_evals[i].success) {
                        // passable優先、スコア比較（passable=falseはスコア半減で重み付け）
                        int effective_score = s_scan_evals[i].score;
                        if (!s_scan_evals[i].passable) {
                            effective_score /= 2;
                        }
                        if (effective_score > best_score) {
                            best_score = effective_score;
                            best_idx = i;
                        }
                    }
                }

                if (best_idx < 0 || best_score < 25) {
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
            // 旋回中もコマンドを毎ループ継続送信
            send_command(s_turn_spin_right ? CmdSpinRight : CmdSpinLeft);

            const int64_t now_us = esp_timer_get_time();
            const float dt = (now_us - s_turn_last_us) / 1000000.0f;
            s_turn_last_us = now_us;

            float gx = 0, gy = 0, gz = 0;
            if (M5.Imu.getGyro(&gx, &gy, &gz)) {
                // CoreS3直立時の旋回主成分はY軸。姿勢や首の傾きによる分散を吸収するため3軸ノルムを算出
                float omega = std::sqrt(gx * gx + gy * gy + gz * gz);
                // 静止時のノイズ・ドリフトを除去（不感帯: 8.0 deg/s未満はカット）
                if (omega < 8.0f) {
                    omega = 0.0f;
                } else if (omega > 500.0f) {
                    omega = 500.0f; // 衝撃ショックによる異常値スパイクをクランプ
                }
                s_turn_integrated_deg += omega * dt;
            }

            const uint32_t turn_elapsed_ms = now_ms - s_state_start_ms;
            // モーター停止コマンド送信から完全停止までの慣性・通信遅延を考慮した先行停止角
            // 45度旋回（斜め）: 加速過渡期のためオーバーシュートは約10度（停止閾値35度、ショートせず45〜47度に着地）
            // 90度旋回（直角）: 定常速度のためオーバーシュート19.5度（停止閾値70.5度、90〜91度に着地）
            // 180度旋回（Uターン）: 最高速定常時のためオーバーシュート約19.5度（停止閾値160.5度）
            float stop_threshold_deg = 0.0f;
            if (s_target_turn_deg <= 45.0f) {
                stop_threshold_deg = s_target_turn_deg * (35.0f / 45.0f); // 45度時: 35.0度
            } else if (s_target_turn_deg <= 90.0f) {
                const float t = (s_target_turn_deg - 45.0f) / 45.0f;
                const float overshoot = 10.0f + t * (19.5f - 10.0f);     // 10.0度〜19.5度へ線形補間
                stop_threshold_deg = s_target_turn_deg - overshoot;      // 90度時: 70.5度
            } else {
                stop_threshold_deg = s_target_turn_deg - 19.5f;          // 180度時: 160.5度
            }

            // 実測角速度に基づく旋回所要時間: 90度なら約1080ms、45度なら約540ms、180度なら約2160ms
            const uint32_t target_duration_ms = static_cast<uint32_t>((s_target_turn_deg / 90.0f) * 1080.0f);
            // 最低旋回時間ガード
            const uint32_t min_turn_ms = static_cast<uint32_t>((s_target_turn_deg / 90.0f) * 500.0f);

            // 旋回テレメトリログ (150msごと)
            static uint32_t s_last_turn_log_ms = 0;
            if (now_ms - s_last_turn_log_ms >= 150) {
                s_last_turn_log_ms = now_ms;
                ESP_LOGI(kTag, "Turning: elapsed=%u ms, integrated=%.1f/%.1f deg (stop_thresh=%.1f), gyro=[%.1f, %.1f, %.1f]",
                         turn_elapsed_ms, s_turn_integrated_deg, s_target_turn_deg, stop_threshold_deg, gx, gy, gz);
            }

            // 完了条件: 最低旋回時間を経過しており、かつ（ジャイロが先行停止角度に達した、または標準所要時間が経過した）
            const bool turn_finished = (turn_elapsed_ms >= min_turn_ms) &&
                                       (s_turn_integrated_deg >= stop_threshold_deg || turn_elapsed_ms >= target_duration_ms);

            // 安全上限ガード: 大回り（オーバーシュート）を防止するための最大時間キャップ
            const uint32_t max_turn_limit_ms = static_cast<uint32_t>((s_target_turn_deg / 90.0f) * 1300.0f);
            if (turn_finished || (turn_elapsed_ms >= max_turn_limit_ms)) {
                send_command(CmdStop);
                ESP_LOGI(kTag, "Turn complete: elapsed=%u ms, integrated=%.1f deg (target=%.1f deg, thresh=%.1f deg).",
                         turn_elapsed_ms, s_turn_integrated_deg, s_target_turn_deg, stop_threshold_deg);

                // 車体が新進路へ旋回完了したため、首を正面（0度）に戻して新進路をまっすぐ見据える
                state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);

                if (s_drive_type == DriveType::SonicOnly) {
                    // 自律運転（距離センサー）: 旋回後の前方距離確認ステートへ
                    ESP_LOGI(kTag, "SonicOnly: Entering VerifySonicOnly state.");
                    s_drive_state = AutoDriveState::VerifySonicOnly;
                    s_state_start_ms = now_ms;
                } else {
                    state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                    say_step(speech, state, "カメラで確認中", U"じゃまなものわ、ないかな", 2500);
                    s_drive_state = AutoDriveState::VerifyCamera;
                    s_state_start_ms = now_ms;
                }
            }
            break;
        }

        case AutoDriveState::VerifySonicOnly: {
            send_command(CmdStop);
            // 旋回直後の車体静止待ちおよび超音波測定値の安定化（500ms）
            if (now_ms - s_state_start_ms >= 500) {
                // 前方の障害物有無を確認 (検知しきい値内か)
                const bool has_obstacle = (dist_mm > 0 && dist_mm <= scan_threshold_mm);
                ESP_LOGI(kTag, "VerifySonicOnly: dist=%u mm, threshold=%u mm, obstacle=%d, retried=%d",
                         dist_mm, scan_threshold_mm, has_obstacle, s_sonic_retried);

                if (!has_obstacle) {
                    // 前方に障害物なし（クリア）：前進スタート！
                    ESP_LOGI(kTag, "SonicOnly: Path clear! Resuming forward drive.");
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
                    say_step(speech, state, "よし！クリア", U"もんだいなし、れっつごー", 1500);
                    send_command(CmdForward);
                    s_drive_state = AutoDriveState::Forward;
                    s_state_start_ms = now_ms;
                    s_next_drive_speech_ms = now_ms + 4000 + (esp_random() % 4001);
                } else {
                    // 前方にまだ障害物がある場合
                    if (!s_sonic_retried) {
                        // 1回目の旋回後なら、再度左90度旋回（計180度Uターンで元来た道へ戻る）
                        ESP_LOGW(kTag, "SonicOnly: Front still blocked. Executing 2nd 90 deg turn (U-turn).");
                        s_sonic_retried = true;
                        state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                        say_step(speech, state, "まだ行き止まり！", U"まだ、ゆきどまりだ、もういっかい、まがるよ", 2000);
                        s_target_turn_deg = 90.0f;
                        s_turn_spin_right = false; // 再度左90度回転
                        s_turn_integrated_deg = 0.0f;
                        s_turn_last_us = esp_timer_get_time();
                        send_command(CmdSpinLeft);
                        s_drive_state = AutoDriveState::Turning;
                        s_state_start_ms = now_ms;
                    } else {
                        // 2回旋回しても障害物がある場合（袋小路）：さらに左に回って抜け道を探す
                        ESP_LOGW(kTag, "SonicOnly: Still blocked after U-turn. Turning left again.");
                        s_target_turn_deg = 90.0f;
                        s_turn_spin_right = false;
                        s_turn_integrated_deg = 0.0f;
                        s_turn_last_us = esp_timer_get_time();
                        send_command(CmdSpinLeft);
                        s_drive_state = AutoDriveState::Turning;
                        s_state_start_ms = now_ms;
                    }
                }
            }
            break;
        }

        case AutoDriveState::VerifyCamera:
            // 旋回直後の手ブレ静止待ち (800ms) かつ 発話終了後1秒待機
            if (now_ms - s_state_start_ms >= 800 && !is_speech_cooling_down(now_ms, speech)) {
                const uint16_t current_dist = state.driving.distance_mm.load(std::memory_order_relaxed);
                const uint16_t verify_limit_mm = scan_threshold_mm + 20;

                ESP_LOGI(kTag, "Verifying new path with camera view (front_check, try %d/3, sonic=%u mm)...",
                         s_verify_retry_count + 1, current_dist);
                VlmEvaluation eval = VlmClient::evaluate_current_view("正面確認");
                if (!eval.success) {
                    s_verify_retry_count++;
                    ESP_LOGW(kTag, "Front check VLM failed (retry %d/3)", s_verify_retry_count);
                    if (s_verify_retry_count < 3) {
                        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                        state.set_balloon_text("正面確認 再試行中…", 1500);
                        s_state_start_ms = now_ms;
                        break;
                    } else {
                        // VLM失敗でも超音波センサーで十分に前方クリア(> verify_limit_mm)なら走行を優先！
                        if (current_dist > verify_limit_mm) {
                            ESP_LOGW(kTag, "VLM failed but ultrasonic clear (%u mm). Proceeding anyway.", current_dist);
                            eval.passable = true;
                            eval.score = 60;
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
                }
                s_verify_retry_count = 0;

                // 複合判定:
                // カメラが極めて低スコア (score < 25) かつ 超音波センサーでも前方に壁を検知 (<= verify_limit_mm) している場合のみ障害物とみなす。
                // 超音波センサーで前方が抜けている（400mm以上等）なら、カメラの遠景・陰影の誤判定として前進を優先する。
                const bool sonic_blocked = (current_dist > 0 && current_dist <= verify_limit_mm);
                if ((!eval.passable && eval.score < 25) && sonic_blocked) {
                    ESP_LOGW(kTag, "Obstacle confirmed by both camera (score=%d) and sonic (%u mm). Rescanning...",
                             eval.score, current_dist);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    say_step(speech, state, "障害物あり！", U"しょうがいぶつ、はっけん", 2500);
                    s_drive_state = AutoDriveState::StartScan;
                    s_state_start_ms = now_ms;
                } else {
                    ESP_LOGI(kTag, "Camera path clear (score=%d, passable=%d, sonic=%u mm). Resuming forward drive.",
                             eval.score, eval.passable, current_dist);
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

        case AutoDriveState::VerifySonic:
            // 互換性のため残すが、VerifyCameraから直接Forwardに移行可能
            s_drive_state = AutoDriveState::Forward;
            break;

        case AutoDriveState::ErrorHold:
            send_command(CmdStop);
            state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
            state.face.bg_color.store(0xF800u, std::memory_order_relaxed); // エラー停止中は赤色を維持
            // 画面タップがあれば復帰
            if (M5.Touch.getCount() > 0) {
                if (s_init_check_failed) {
                    ESP_LOGI(kTag, "Screen tapped in ErrorHold (init check failed). Returning to mode select...");
                    s_init_check_failed = false;
                    state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 標準黒
                    state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                    mode_select::show();
                    s_drive_state = AutoDriveState::Standby;
                } else {
                    ESP_LOGI(kTag, "Screen tapped in ErrorHold. Resuming scan sequence...");
                    state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 標準黒
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    say_step(speech, state, "再探索するよ", U"もういちど、さがすよ", 1500);
                    s_drive_state = AutoDriveState::StartScan;
                }
                s_state_start_ms = now_ms;
            }
            break;
        }
    }

    // 走行中フラグの更新（LED点灯連動用: 起動時テスト＆自律走行常時点灯）
    bool is_moving_now = false;
    if (s_drive_state == AutoDriveState::InitWait) {
        // 起動時LED点灯テスト: サーボ自己診断中（起動からモード選択画面が出るまで）は本体LEDも点灯
        is_moving_now = true;
    } else if (current_mode == SharedState::Driving::Mode::Manual) {
        // JoyC手動操縦モード: モード選択済み(s_mode_selected)なら点灯
        is_moving_now = s_mode_selected || state.driving.joy_active.load(std::memory_order_relaxed);
    } else {
        // 自律走行モード: ユーザー要望により、モード選択後はモーター動作時だけでなく常時点灯
        // （探索中・待機中・旋回中・確認中も含めて常時点灯。エラー停止中のみ消灯）
        is_moving_now = s_mode_selected && (s_drive_state != AutoDriveState::ErrorHold);
    }
    state.driving.is_moving.store(is_moving_now, std::memory_order_relaxed);
}

void AtomicMotionClient::toggle_start_stop(SharedState& state, Speech& speech)
{
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const auto current_mode = state.driving.mode.load(std::memory_order_relaxed);
    if (current_mode != SharedState::Driving::Mode::Autonomous) {
        return;
    }

    // チャタリング・二重トグル防止 (500ms デバウンス)
    if (now_ms - s_last_toggle_ms < 500) {
        ESP_LOGI(kTag, "toggle_start_stop: Debounced (within 500ms)");
        return;
    }
    s_last_toggle_ms = now_ms;

    if (s_drive_state == AutoDriveState::Standby || s_drive_state == AutoDriveState::InitWait) {
        // 停止待機中から「スタートするよ」と宣言して、発話完了後に前進開始
        ESP_LOGI(kTag, "Tap: Starting autonomous driving (announcing start)!");
        send_command(CmdStop); // まだ走らない

        // 前の発話があれば確実に停止してからスタート発話を開始
        speech.stop();
        s_is_regular_driving_speech = false;
        s_has_pending_speech = false;
        s_pending_speech_reading.clear();

        state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
        state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
        say_step(speech, state, "スタートするよ", U"すたーと、するよ", 2500);

        s_start_speech_active = false;
        s_start_delay_ms = 0;
        s_drive_state = AutoDriveState::StartWait;
        s_state_start_ms = now_ms;
    } else {
        // 走行中・探索中・スタート待機中から即座に停止し、「停止するよ」と発話
        ESP_LOGI(kTag, "Tap: Stopping autonomous driving (announcing stop)!");
        send_command(CmdStop);
        speech.stop();
        s_is_regular_driving_speech = false;
        s_has_pending_speech = false;
        s_pending_speech_reading.clear();

        state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
        state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
        say_step(speech, state, "停止するよ", U"ていし、するよ", 2500);
        s_drive_state = AutoDriveState::Standby;
        s_state_start_ms = now_ms;
    }
}

void AtomicMotionClient::set_drive_type(DriveType type, SharedState& state)
{
    s_drive_type = type;
    s_mode_selected = true;
    s_drive_type_speech_pending = true;
    s_mode_selected_ms = esp_timer_get_time() / 1000;
    s_standby_prompt_shown = false;

    if (type == DriveType::JoyCManual) {
        state.driving.mode.store(SharedState::Driving::Mode::Manual, std::memory_order_relaxed);
        set_mode(SharedState::Driving::Mode::Manual);
        state.set_balloon_text("JoyC操作モード", 3500);
        state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
        state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
        send_command(CmdStop);
    } else if (type == DriveType::SonicOnly) {
        state.driving.mode.store(SharedState::Driving::Mode::Autonomous, std::memory_order_relaxed);
        set_mode(SharedState::Driving::Mode::Autonomous);
        state.set_balloon_text("距離センサーモード", 3500);
        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
        state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
        s_drive_state = AutoDriveState::Standby;
        send_command(CmdStop);
    } else if (type == DriveType::SonicCamera) {
        state.driving.mode.store(SharedState::Driving::Mode::Autonomous, std::memory_order_relaxed);
        set_mode(SharedState::Driving::Mode::Autonomous);
        state.set_balloon_text("カメラ確認中…", 3000);
        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
        state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
        s_init_check_failed = false;
        s_drive_state = AutoDriveState::CameraCheck;
        send_command(CmdStop);
    }
}

AtomicMotionClient::DriveType AtomicMotionClient::get_drive_type()
{
    return s_drive_type;
}

bool AtomicMotionClient::is_mode_selected()
{
    return s_mode_selected;
}

bool AtomicMotionClient::is_running()
{
    return s_mode_selected &&
           (s_drive_state != AutoDriveState::Standby &&
            s_drive_state != AutoDriveState::InitWait &&
            s_drive_state != AutoDriveState::ErrorHold);
}

} // namespace stackchan::app
