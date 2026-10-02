// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"
#include "speech.hpp"
#include "wifi_sta.hpp"

#include <cstdio>
#include <cmath>
#include "avatar/expression.hpp"
#include "board/ltr553.hpp"
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

static const ScanPoint kScanPoints[2] = {
    { -90.0f, "右(90度)", "右を見てみるね…", U"みぎわ、あいてるかな" },
    { +90.0f, "左(90度)", "左を見てみるね…", U"ひだりわ、どうだろう" }
};

AutoDriveState s_drive_state = AutoDriveState::InitWait;
uint32_t s_state_start_ms = 0;
int s_scan_index = 0;
int s_eval_retry_count = 0;
int s_verify_retry_count = 0;
VlmEvaluation s_scan_evals[2];
bool s_obstacle_speech_active = false;
bool s_start_speech_active = false;
uint32_t s_start_delay_ms = 0;
uint32_t s_last_toggle_ms = 0;
AtomicMotionClient::DriveType s_drive_type = AtomicMotionClient::DriveType::SonicOnly;
bool s_mode_selected = true;
bool s_drive_type_speech_pending = false;
uint32_t s_mode_selected_ms = 0;
bool s_standby_prompt_shown = false;
bool s_sonic_retried = false;

// 収束型IMU旋回制御
enum class TurnSubStep {
    PulseRotate,   // キャタピラ回転パルス送信中
    WaitSettle,    // 停止後の静止・安定待ち（慣性滑り積算含む）
    Evaluate,      // 静止角度測定・差分評価・次回補正計算
};

struct TurnSequence {
    float target_deg = 90.0f;           // 目標角度 (90.0 or 180.0)
    bool initial_right = true;          // 初回旋回方向 (true: 右, false: 左)
    float accumulated_deg = 0.0f;       // 初回目標方向への累積旋回角度
    float last_step_start_deg = 0.0f;   // 今回のパルス開始時の累積角度
    uint32_t pulse_duration_ms = 0;     // 今回のパルス回転時間 (ms)
    bool current_spin_right = true;     // 今回のパルス回転方向 (右/左)
    float effective_deg_per_ms = 0.100f;// 推定実効角速度 (deg/ms, 実測約100 deg/s)
    int iteration = 0;                  // 補正回数 (0: 初回大回転, 1..: 微調整)
    TurnSubStep sub_step = TurnSubStep::PulseRotate;
};

// ======================================================================
// オフライン走行ログ（RAMリングバッファ：リブート時に自動クリア）
// ======================================================================
struct TurnHistoryEntry {
    uint32_t turn_id = 0;
    uint32_t timestamp_s = 0;
    float target_deg = 0.0f;
    bool spin_right = true;
    uint32_t init_pulse_ms = 0;
    float init_accum_deg = 0.0f;    // 初回パルス終了直後の累積角度
    uint32_t settle_ms = 0;          // 静止待機時間
    float settled_deg = 0.0f;       // 静止完了時の角度
    float error_deg = 0.0f;         // 静止完了時の誤差
    int iterations = 0;              // 0: 初回で合格, 1: 微調整実施
    uint32_t corr_pulse_ms = 0;      // 微調整パルス時間
    bool corr_spin_right = true;    // 微調整方向
    float final_deg = 0.0f;         // 最終確定角度
};

static constexpr size_t kMaxTurnHistory = 32;
static TurnHistoryEntry s_turn_history[kMaxTurnHistory];
static size_t s_turn_history_count = 0;
static uint32_t s_total_turns_counter = 0;
static TurnHistoryEntry s_current_turn;

void record_turn_history(const TurnHistoryEntry& entry)
{
    s_turn_history[s_turn_history_count % kMaxTurnHistory] = entry;
    s_turn_history_count++;
}

void dump_turn_history()
{
    if (s_turn_history_count == 0) {
        ESP_LOGI(kTag, "=== [TURN HISTORY]: No turns recorded in this session yet ===");
        return;
    }

    ESP_LOGI(kTag, "==================== [OFFLINE TURN HISTORY (%zu turns recorded)] ====================", s_turn_history_count);
    const size_t start = (s_turn_history_count > kMaxTurnHistory) ? (s_turn_history_count - kMaxTurnHistory) : 0;
    for (size_t i = start; i < s_turn_history_count; ++i) {
        const auto& e = s_turn_history[i % kMaxTurnHistory];
        if (e.iterations == 0) {
            ESP_LOGI(kTag, "#%u [%us] Target: %.1f deg (%s) | InitPulse: %ums -> Cutoff: %.1f deg -> Settled: %.1f deg (Err: %+.1f deg) => PERFECT (0 corr)",
                     static_cast<unsigned>(e.turn_id), static_cast<unsigned>(e.timestamp_s), e.target_deg,
                     e.spin_right ? "RIGHT" : "LEFT", static_cast<unsigned>(e.init_pulse_ms),
                     e.init_accum_deg, e.settled_deg, e.error_deg);
        } else {
            ESP_LOGI(kTag, "#%u [%us] Target: %.1f deg (%s) | InitPulse: %ums -> Settled: %.1f deg (Err: %+.1f deg) => CORR: %s %ums -> Final: %.1f deg (Err: %+.1f deg)",
                     static_cast<unsigned>(e.turn_id), static_cast<unsigned>(e.timestamp_s), e.target_deg,
                     e.spin_right ? "RIGHT" : "LEFT", static_cast<unsigned>(e.init_pulse_ms),
                     e.settled_deg, e.error_deg,
                     e.corr_spin_right ? "RIGHT" : "LEFT", static_cast<unsigned>(e.corr_pulse_ms),
                     e.final_deg, e.target_deg - e.final_deg);
        }
    }
    ESP_LOGI(kTag, "======================================================================================");
}

static float s_target_turn_deg = 0.0f;
static bool s_turn_spin_right = true;
static int64_t s_turn_last_us = 0;
static TurnSequence s_turn_seq;

void start_turning(SharedState& state, float target_deg, bool spin_right, uint32_t now_ms)
{
    s_target_turn_deg = target_deg;
    s_turn_spin_right = spin_right;

    s_turn_seq.target_deg = target_deg;
    s_turn_seq.initial_right = spin_right;
    s_turn_seq.accumulated_deg = 0.0f;
    s_turn_seq.last_step_start_deg = 0.0f;
    s_turn_seq.effective_deg_per_ms = 0.100f;
    s_turn_seq.iteration = 0;

    // 初回パルス時間: 停止後の慣性滑り（約7〜9度）を見越し、目標角度から先行して停止
    // 90度目標時: (90 - 7) / 0.100 = 830ms (実機で約82度まで回転し滑りで88〜92度に到達)
    // 180度目標時: (180 - 15) / 0.100 = 1650ms
    const float initial_target_deg = (target_deg <= 90.0f) ? (target_deg - 7.0f) : (target_deg - 15.0f);
    s_turn_seq.pulse_duration_ms = static_cast<uint32_t>(initial_target_deg / s_turn_seq.effective_deg_per_ms);
    s_turn_seq.current_spin_right = spin_right;
    s_turn_seq.sub_step = TurnSubStep::PulseRotate;

    // 新規ログエントリの初期化
    s_total_turns_counter++;
    s_current_turn = TurnHistoryEntry{};
    s_current_turn.turn_id = s_total_turns_counter;
    s_current_turn.timestamp_s = now_ms / 1000;
    s_current_turn.target_deg = target_deg;
    s_current_turn.spin_right = spin_right;
    s_current_turn.init_pulse_ms = s_turn_seq.pulse_duration_ms;

    s_turn_last_us = esp_timer_get_time();
    AtomicMotionClient::send_command(spin_right ? AtomicMotionClient::CmdSpinRight : AtomicMotionClient::CmdSpinLeft);

    // 首は正面固定
    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);

    s_drive_state = AutoDriveState::Turning;
    s_state_start_ms = now_ms;

    ESP_LOGI(kTag, "Start turn sequence #%u: target=%.1f deg, spin_%s, initial pulse=%u ms",
             static_cast<unsigned>(s_current_turn.turn_id),
             target_deg, spin_right ? "right" : "left", static_cast<unsigned>(s_turn_seq.pulse_duration_ms));
}
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

    // JoyC inactive
    out_status.joy_active = false;

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

    // モード設定時の案内発話保留があれば再生
    if (s_drive_type_speech_pending && !speech.is_speaking()) {
        s_drive_type_speech_pending = false;
        say_step(speech, state, "距離センサーモード", U"きょりせんさー、もーど", 3500);
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
            // 起動後 15 秒（サーボセルフテスト完了）待機してからStandby（タップでスタート待機）へ移行 (Atom接続時のみ)
            if (now_ms >= 15000 && is_connected()) {
                send_command(CmdStop);
                state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
                ESP_LOGI(kTag, "Servo self-test complete & Atom connected. Direct to Standby.");
                s_mode_selected = true;
                s_standby_prompt_shown = false;
                s_drive_state = AutoDriveState::Standby;
                s_state_start_ms = now_ms;
            }
            break;

        case AutoDriveState::Standby: {
            // 停止待機中: モーター停止維持。画面タップでStartWaitへ移行する
            send_command(CmdStop);

            // オフライン走行後にUSB接続された際、直近の旋回履歴をPCへ自動出力（3秒ごと）
            static uint32_t s_last_history_dump_ms = 0;
            if (s_turn_history_count > 0 && (now_ms - s_last_history_dump_ms >= 3000)) {
                s_last_history_dump_ms = now_ms;
                dump_turn_history();
            }
            break;
        }

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
                        state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                        state.set_balloon_text("左へ方向転換！", 1500);
                        start_turning(state, 90.0f, /*spin_right=*/false, now_ms);
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
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    state.set_balloon_text("左へ方向転換！", 1500);
                    start_turning(state, 90.0f, /*spin_right=*/false, now_ms);
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

            // 近接センサ(LTR-553ALS-WA)の読み取り
            ProximityInfo prox_info;
            auto prox_opt = board::Ltr553Proximity::read();
            if (prox_opt.has_value()) {
                prox_info.available = true;
                prox_info.ps_raw = prox_opt->ps_raw;
                prox_info.obstacle_near = prox_opt->obstacle_near;
                ESP_LOGI(kTag, "LTR-553 proximity for %s: raw=%u, obstacle_near=%d",
                         pt.name, prox_info.ps_raw, prox_info.obstacle_near);
            } else {
                ESP_LOGW(kTag, "LTR-553 proximity read failed for %s", pt.name);
            }

            VlmEvaluation eval = VlmClient::evaluate_current_view(pt.name, &prox_info);

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
            std::snprintf(buf, sizeof(buf), "%s: %d点 (%s%s)",
                          pt.name, eval.score, eval.passable ? "OK" : "NG",
                          eval.proximity.obstacle_near ? ",近接NG" : "");
            state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
            state.set_balloon_text(buf, 2000);

            s_scan_index++;
            if (s_scan_index < 2) {
                // 次の方向（左90度）へ首を向ける（※まだ喋らない！）
                ESP_LOGI(kTag, "Next scan target %d: %s (yaw=%.1f deg) - rotating head first",
                         s_scan_index, kScanPoints[s_scan_index].name, kScanPoints[s_scan_index].yaw_deg);
                state.servo.target_yaw_deg.store(kScanPoints[s_scan_index].yaw_deg, std::memory_order_relaxed);
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                state.set_balloon_text(kScanPoints[s_scan_index].msg, 2500);

                // 首の回転待ちステートへ遷移
                s_drive_state = AutoDriveState::ScanHeadMoving;
                s_state_start_ms = now_ms;
            } else {
                // 左右2方向完了、首を正面に戻して判定へ
                ESP_LOGI(kTag, "Scan complete for both 90 deg directions. Returning head to center.");
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
                // 左右2方向 (0: 右90°, 1: 左90°) の評価を集計
                // 近接センサで障害物がある場合や passable=false の場合はNG
                const bool right_ok = s_scan_evals[0].success && s_scan_evals[0].passable &&
                                      !s_scan_evals[0].proximity.obstacle_near && (s_scan_evals[0].score >= 40);
                const bool left_ok  = s_scan_evals[1].success && s_scan_evals[1].passable &&
                                      !s_scan_evals[1].proximity.obstacle_near && (s_scan_evals[1].score >= 40);

                int chosen_idx = -1;
                if (right_ok && left_ok) {
                    // 両方OKの場合はスコアが高い方を選択（同点なら右優先）
                    chosen_idx = (s_scan_evals[0].score >= s_scan_evals[1].score) ? 0 : 1;
                } else if (right_ok) {
                    chosen_idx = 0;
                } else if (left_ok) {
                    chosen_idx = 1;
                }

                if (chosen_idx < 0) {
                    // 左右どちらもNGだった場合は最初の進行方向から180度反転 (Uターン)
                    ESP_LOGW(kTag, "Both left and right blocked (R: pass=%d/score=%d/prox=%d, L: pass=%d/score=%d/prox=%d). Executing 180 deg turnaround.",
                             s_scan_evals[0].passable, s_scan_evals[0].score, s_scan_evals[0].proximity.obstacle_near,
                             s_scan_evals[1].passable, s_scan_evals[1].score, s_scan_evals[1].proximity.obstacle_near);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Doubt), std::memory_order_relaxed);
                    say_step(speech, state, "行き止まり！", U"ゆきどまりだ、ゆーたーんするよ", 2500);
                    start_turning(state, 180.0f, /*spin_right=*/true, now_ms);
                } else {
                    // 良い方を選択して90度旋回
                    const auto& best_pt = kScanPoints[chosen_idx];
                    ESP_LOGI(kTag, "Chosen direction: %s (score=%d)", best_pt.name, s_scan_evals[chosen_idx].score);
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%sへ進路変更！", best_pt.name);
                    state.set_balloon_text(buf, 2000);
                    state.face.expression.store(static_cast<int>(avatar::Expression::Happy), std::memory_order_relaxed);
                    say_step(speech, state, "方向転換するよ", U"しんこうほうこう、へんこう", 2000);

                    const bool spin_right = (chosen_idx == 0); // 0: 右90°, 1: 左90°
                    start_turning(state, 90.0f, spin_right, now_ms);
                }
            }
            break;

        case AutoDriveState::Turning: {
            const int64_t now_us = esp_timer_get_time();
            const float dt = (now_us - s_turn_last_us) / 1000000.0f;
            s_turn_last_us = now_us;

            // 1. IMU角速度の取得と目標方向への累積角度更新
            float gx = 0, gy = 0, gz = 0;
            float abs_gy = 0.0f;
            if (M5.Imu.getGyro(&gx, &gy, &gz)) {
                // CoreS3垂直搭載時、車体ヨー回転はY軸。
                // 実機測定: ay = +0.98g なので +Y 軸は天頂方向（上向き）。
                // 右手系角速度:
                // 右旋回(CW)時は gy < 0
                // 左旋回(CCW)時は gy > 0
                // したがって、原点0度から目標方向への正の累積角度(accumulated_deg)を得るには:
                // initial_right == true (右旋回目標) のときは -gy が正の旋回速度
                // initial_right == false (左旋回目標) のときは +gy が正の旋回速度
                abs_gy = std::abs(gy);
                if (abs_gy < 3.0f) {
                    gy = 0.0f; // 静止ノイズ不感帯カット (実測静止バイアス約0.75 deg/s)
                } else if (abs_gy > 500.0f) {
                    gy = (gy > 0) ? 500.0f : -500.0f; // 衝撃スパイク保護
                }
                const float omega = s_turn_seq.initial_right ? -gy : +gy;
                s_turn_seq.accumulated_deg += omega * dt;
            }

            // 旋回テレメトリログ (150msごと)
            static uint32_t s_last_turn_log_ms = 0;
            if (now_ms - s_last_turn_log_ms >= 150) {
                s_last_turn_log_ms = now_ms;
                ESP_LOGI(kTag, "Turning: step=%d, iter=%d, accumulated=%.1f/%.1f deg, gyro=[%.1f, %.1f, %.1f]",
                         static_cast<int>(s_turn_seq.sub_step), s_turn_seq.iteration,
                         s_turn_seq.accumulated_deg, s_turn_seq.target_deg, gx, gy, gz);
            }

            // 2. 収束型サブステップ制御
            switch (s_turn_seq.sub_step) {
            case TurnSubStep::PulseRotate: {
                // キャタピラ回転コマンドを定期送信
                send_command(s_turn_seq.current_spin_right ? CmdSpinRight : CmdSpinLeft);

                const uint32_t pulse_elapsed_ms = now_ms - s_state_start_ms;
                // 設定されたパルス時間が経過したらキャタピラ停止
                if (pulse_elapsed_ms >= s_turn_seq.pulse_duration_ms) {
                    send_command(CmdStop);
                    if (s_turn_seq.iteration == 0) {
                        s_current_turn.init_accum_deg = s_turn_seq.accumulated_deg;
                    }
                    ESP_LOGI(kTag, "Turn pulse finished (%u ms). Accumulated=%.1f deg. Entering WaitSettle.",
                             pulse_elapsed_ms, s_turn_seq.accumulated_deg);
                    s_turn_seq.sub_step = TurnSubStep::WaitSettle;
                    s_state_start_ms = now_ms;
                }
                break;
            }

            case TurnSubStep::WaitSettle: {
                // キャタピラ停止コマンドを維持
                send_command(CmdStop);

                // 停止後の慣性滑りと車体振動が静止・安定するまで待機
                // ユーザー要望: 旋回終了後、1秒間しっかり静止待機して車体とIMUが完全静止してから判定する
                const uint32_t settle_elapsed = now_ms - s_state_start_ms;
                if (settle_elapsed >= 1000) {
                    if (s_turn_seq.iteration == 0) {
                        s_current_turn.settle_ms = settle_elapsed;
                        s_current_turn.settled_deg = s_turn_seq.accumulated_deg;
                        s_current_turn.error_deg = s_turn_seq.target_deg - s_turn_seq.accumulated_deg;
                    }
                    s_turn_seq.sub_step = TurnSubStep::Evaluate;
                }
                break;
            }

            case TurnSubStep::Evaluate: {
                send_command(CmdStop);

                // 原点(0度)から静止時点での累積角度と目標角度との差分を計算
                // error > 0: 不足（当初の向きへさらに回転が必要）
                // error < 0: 行き過ぎ・オーバーシュート（逆向きへ戻す補正回転が必要）
                const float error = s_turn_seq.target_deg - s_turn_seq.accumulated_deg;
                const float step_delta = std::abs(s_turn_seq.accumulated_deg - s_turn_seq.last_step_start_deg);

                // 今回のステップでの実測角速度（deg/ms）を推定して学習・更新
                if (s_turn_seq.pulse_duration_ms > 0 && step_delta > 2.0f) {
                    float measured_rate = step_delta / static_cast<float>(s_turn_seq.pulse_duration_ms);
                    measured_rate = std::clamp(measured_rate, 0.060f, 0.180f);
                    s_turn_seq.effective_deg_per_ms = 0.6f * s_turn_seq.effective_deg_per_ms + 0.4f * measured_rate;
                }

                // 許容誤差: 目標90度に対して±14.0度 (76〜104度) 以内、または微調整1回完了で即座に完了
                // 初回旋回でほぼ90度（76〜104度）に着地していれば、余計な追加回転を一切起こさず即座に前進へ移行する
                if (std::abs(error) <= 14.0f || s_turn_seq.iteration >= 1) {
                    s_current_turn.iterations = s_turn_seq.iteration;
                    s_current_turn.final_deg = s_turn_seq.accumulated_deg;
                    record_turn_history(s_current_turn);
                    dump_turn_history();

                    ESP_LOGI(kTag, "Turn CONVERGED! Target=%.1f deg, Final=%.1f deg (error=%.1f deg) in %d iterations (rate=%.4f deg/ms)",
                             s_turn_seq.target_deg, s_turn_seq.accumulated_deg, error, s_turn_seq.iteration, s_turn_seq.effective_deg_per_ms);

                    // 首を正面（0度）に戻す
                    state.servo.target_yaw_deg.store(0.0f, std::memory_order_relaxed);

                    if (s_drive_type == DriveType::SonicOnly) {
                        ESP_LOGI(kTag, "SonicOnly: Entering VerifySonicOnly state.");
                        s_drive_state = AutoDriveState::VerifySonicOnly;
                        s_state_start_ms = now_ms;
                    } else {
                        state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                        say_step(speech, state, "カメラで確認中", U"じゃまなものわ、ないかな", 2500);
                        s_drive_state = AutoDriveState::VerifyCamera;
                        s_state_start_ms = now_ms;
                    }
                    break;
                }

                // どうしても14度以上ズレていた場合のみ、最大1回だけごくわずかな極小チョン当て（35〜55ms）
                s_turn_seq.iteration++;
                s_turn_seq.last_step_start_deg = s_turn_seq.accumulated_deg;

                // error > 0 (不足) なら当初の向き、error < 0 (行き過ぎ) なら逆向き
                if (error > 0) {
                    s_turn_seq.current_spin_right = s_turn_seq.initial_right;
                } else {
                    s_turn_seq.current_spin_right = !s_turn_seq.initial_right;
                }

                // 微小補正時間 (ms) の計算
                float needed_deg = std::abs(error);
                if (needed_deg > 6.0f) {
                    needed_deg -= 3.0f; // 停止時の滑りを考慮
                }
                uint32_t corr_ms = static_cast<uint32_t>(needed_deg / s_turn_seq.effective_deg_per_ms);
                // 補正パルスは極小のチョン当て（35〜55ms）に制限し、過剰回転を完全に防止
                corr_ms = std::clamp<uint32_t>(corr_ms, 35, 55);
                s_turn_seq.pulse_duration_ms = corr_ms;

                s_current_turn.corr_pulse_ms = corr_ms;
                s_current_turn.corr_spin_right = s_turn_seq.current_spin_right;

                ESP_LOGI(kTag, "Turn Eval [iter %d]: target=%.1f, accumulated=%.1f, error=%.1f deg -> corr_dir=%s, duration=%u ms (rate=%.4f deg/ms)",
                         s_turn_seq.iteration, s_turn_seq.target_deg, s_turn_seq.accumulated_deg, error,
                         s_turn_seq.current_spin_right ? "RIGHT" : "LEFT",
                         static_cast<unsigned>(corr_ms), s_turn_seq.effective_deg_per_ms);

                s_turn_seq.sub_step = TurnSubStep::PulseRotate;
                s_state_start_ms = now_ms;
                send_command(s_turn_seq.current_spin_right ? CmdSpinRight : CmdSpinLeft);
                break;
            }
            } // switch (sub_step)
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
                        start_turning(state, 90.0f, /*spin_right=*/false, now_ms);
                    } else {
                        // 2回旋回しても障害物がある場合（袋小路）：さらに左に回って抜け道を探す
                        ESP_LOGW(kTag, "SonicOnly: Still blocked after U-turn. Turning left again.");
                        start_turning(state, 90.0f, /*spin_right=*/false, now_ms);
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
                ESP_LOGI(kTag, "Screen tapped in ErrorHold. Resuming standby...");
                s_init_check_failed = false;
                state.face.bg_color.store(0x0000u, std::memory_order_relaxed); // 標準黒
                state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
                s_standby_prompt_shown = false;
                s_drive_state = AutoDriveState::Standby;
                s_state_start_ms = now_ms;
            }
            break;
        }
    }

    // 走行中フラグの更新（LED点灯連動用: 起動時テスト＆自律走行常時点灯）
    bool is_moving_now = false;
    if (s_drive_state == AutoDriveState::InitWait) {
        // 起動時LED点灯テスト: サーボ自己診断中は本体LEDも点灯
        is_moving_now = true;
    } else {
        // 超音波自律走行モード: 常時点灯（エラー停止中のみ消灯）
        is_moving_now = (s_drive_state != AutoDriveState::ErrorHold);
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
    (void)type;
    s_drive_type = DriveType::SonicOnly;
    s_mode_selected = true;
    s_drive_type_speech_pending = false;
    s_mode_selected_ms = esp_timer_get_time() / 1000;
    s_standby_prompt_shown = false;

    state.driving.mode.store(SharedState::Driving::Mode::Autonomous, std::memory_order_relaxed);
    set_mode(SharedState::Driving::Mode::Autonomous);
    state.set_balloon_text("距離センサーモード", 3500);
    state.face.expression.store(static_cast<int>(avatar::Expression::Neutral), std::memory_order_relaxed);
    state.face.bg_color.store(0x0000u, std::memory_order_relaxed);
    s_drive_state = AutoDriveState::Standby;
    send_command(CmdStop);
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
