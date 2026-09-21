// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstdint>
#include <string>

namespace stackchan::app {

struct VlmEvaluation {
    bool success{false};
    bool passable{false};
    int score{0}; // 0..100
    std::string reason;
};

class VlmClient {
public:
    static constexpr const char* kDefaultEndpoint = "http://192.168.11.6:1234/v1/chat/completions";
    static constexpr const char* kDefaultModel = "qwen3.5-9b-vlm";

    // 現在のカメラフレームを取得し、ローカルVLMに送信して指定方向の通行可否とスコアを評価
    static VlmEvaluation evaluate_current_view(const char* direction_label = "front");
};

} // namespace stackchan::app
