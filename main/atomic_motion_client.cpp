// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "atomic_motion_client.hpp"

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace stackchan::app {

namespace {
constexpr const char* kTag = "atomic_client";
constexpr int kI2cTimeoutMs = 50;

bool s_initialized = false;
i2c_master_bus_handle_t s_bus_handle = nullptr;
i2c_master_dev_handle_t s_dev_handle = nullptr;

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

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "i2c_new_master_bus (port 0) failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kDefaultSlaveAddr;
    dev_cfg.scl_speed_hz = 100000;

    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    s_mode_force_sync = true;
    ESP_LOGI(kTag, "AtomicMotion I2C master (driver_ng) initialized on Port A (SDA:%d, SCL:%d)", sda_pin, scl_pin);
    return ESP_OK;
}

esp_err_t AtomicMotionClient::set_mode(SharedState::Driving::Mode mode)
{
    if (!s_initialized || !s_dev_handle) return ESP_ERR_INVALID_STATE;

    const uint8_t val = (mode == SharedState::Driving::Mode::Manual) ? 0x01 : 0x00;
    const uint8_t buf[2] = {0x04, val}; // Register 0x04: ROBOT_MODE
    esp_err_t err = i2c_master_transmit(s_dev_handle, buf, sizeof(buf), kI2cTimeoutMs);
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "Robot mode synchronized to %s", (mode == SharedState::Driving::Mode::Manual) ? "Manual" : "Autonomous");
    } else {
        static int64_t last_err_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_err_us > 3000000) {
            last_err_us = now_us;
            ESP_LOGW(kTag, "Failed to send set_mode: %s (AtomS3 connected on Port A?)", esp_err_to_name(err));
        }
    }
    return err;
}

esp_err_t AtomicMotionClient::send_command(Command cmd)
{
    if (!s_initialized || !s_dev_handle) return ESP_ERR_INVALID_STATE;

    const uint8_t buf[2] = {0x00, static_cast<uint8_t>(cmd)}; // Register 0x00: CMD_MODE
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), kI2cTimeoutMs);
}

esp_err_t AtomicMotionClient::set_motor_speeds(int8_t left, int8_t right)
{
    if (!s_initialized || !s_dev_handle) return ESP_ERR_INVALID_STATE;

    const uint8_t buf[3] = {0x01, static_cast<uint8_t>(left), static_cast<uint8_t>(right)};
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), kI2cTimeoutMs);
}

esp_err_t AtomicMotionClient::set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_initialized || !s_dev_handle) return ESP_ERR_INVALID_STATE;

    const uint8_t buf[4] = {0x20, r, g, b};
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), kI2cTimeoutMs);
}

esp_err_t AtomicMotionClient::read_status(Status& out_status)
{
    if (!s_initialized || !s_dev_handle) return ESP_ERR_INVALID_STATE;

    // Read distance (0x10, 2 bytes), flags (0x12, 1 byte)
    uint8_t reg = 0x10;
    uint8_t data[3] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, 3, kI2cTimeoutMs);
    if (err == ESP_OK) {
        out_status.distance_mm = (static_cast<uint16_t>(data[0]) << 8) | data[1];
        out_status.obstacle_flags = data[2];
    } else {
        return err;
    }

    // Read JoyC active flag (0x05)
    reg = 0x05;
    uint8_t joy_byte = 0;
    err = i2c_master_transmit_receive(s_dev_handle, &reg, 1, &joy_byte, 1, kI2cTimeoutMs);
    if (err == ESP_OK) {
        out_status.joy_active = (joy_byte != 0);
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
    }

    // If Autonomous, run high-level obstacle avoidance controller
    if (current_mode == SharedState::Driving::Mode::Autonomous) {
        const uint16_t dist_mm = state.driving.distance_mm.load(std::memory_order_relaxed);
        const bool obstacle_detected = (dist_mm < 250); // < 25 cm

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
