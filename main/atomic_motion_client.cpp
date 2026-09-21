// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"

#include <cstdio>
#include "avatar/expression.hpp"
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
uint32_t s_auto_drive_state_ms = 0;
int s_auto_step = 0; // 0: Idle/Forward, 1: Obstacle avoiding (turn)
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
        static bool s_last_obstacle = false;
        static uint32_t s_last_balloon_update_ms = 0;

        const bool obstacle = (status.obstacle_flags & 0x01) != 0;
        const uint16_t dist_cm = status.distance_mm / 10;

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
        s_last_obstacle = obstacle;
    }

    // If Autonomous, run high-level obstacle avoidance controller
    if (current_mode == SharedState::Driving::Mode::Autonomous) {
        const uint16_t dist_mm = state.driving.distance_mm.load(std::memory_order_relaxed);
        const bool obstacle_detected = (dist_mm > 0 && dist_mm < 250); // < 25 cm (0 means no reading yet)

        if (s_auto_step == 0) {
            // Normal forward motion
            if (obstacle_detected) {
                // Obstacle! Switch to turn/avoidance
                send_command(CmdStop);
                s_auto_step = 1;
                s_auto_drive_state_ms = now_ms;
            } else {
                send_command(CmdForward);
            }
        } else if (s_auto_step == 1) {
            // Spin right to find an open path
            if (now_ms - s_auto_drive_state_ms < 800) {
                send_command(CmdSpinRight);
            } else {
                // Done spinning, re-evaluate
                send_command(CmdStop);
                if (!obstacle_detected) {
                    s_auto_step = 0;
                } else {
                    s_auto_drive_state_ms = now_ms; // Continue turning if still blocked
                }
            }
        }
    }
}

} // namespace stackchan::app
