// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <esp_err.h>
#include "shared_state.hpp"

namespace stackchan::app {

class Speech;

class AtomicMotionClient {
public:
    static constexpr uint8_t kDefaultSlaveAddr = 0x42;
    static constexpr int kPortASdaPin = 2;
    static constexpr int kPortASclPin = 1;

    enum Command : uint8_t {
        CmdStop      = 0x00,
        CmdForward   = 0x01,
        CmdBackward  = 0x02,
        CmdSpinLeft  = 0x03,
        CmdSpinRight = 0x04,
    };

    struct Status {
        uint16_t distance_mm{9999};
        uint8_t obstacle_flags{0};
        bool joy_active{false};
        uint8_t robot_mode{0};
    };

    static esp_err_t init(int sda_pin = kPortASdaPin, int scl_pin = kPortASclPin);
    static esp_err_t set_mode(SharedState::Driving::Mode mode);
    static esp_err_t send_command(Command cmd);
    static esp_err_t set_motor_speeds(int8_t left, int8_t right);
    static esp_err_t read_status(Status& out_status);
    static esp_err_t set_led_color(uint8_t r, uint8_t g, uint8_t b);

    // Periodic sync helper: synchronizes mode if changed, reads status and updates SharedState.
    // If in autonomous mode, can perform obstacle avoidance driving logic.
    static void tick(SharedState& state, Speech& speech);

    enum class DriveType {
        SonicOnly,    // 自律運転（距離センサーのみ、壁検知で左90度旋回）
        SonicCamera,  // 自律運転（距離＋カメラ、VLM推論）
        JoyCManual,   // JoyC操作（ESPNow）
    };

    static void set_drive_type(DriveType type, SharedState& state);
    static DriveType get_drive_type();
    static bool is_mode_selected();

    // Toggle start / force stop via center screen tap
    static void toggle_start_stop(SharedState& state, Speech& speech);
};

} // namespace stackchan::app
