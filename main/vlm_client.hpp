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

enum class VlmStatus {
    Unknown,     // 未確認（初期状態）
    Checking,    // 接続確認中
    Available,   // 利用可能（VLM OK）
    Unavailable, // 利用不可（未接続・タイムアウト等）
};

class VlmClient {
public:
    static constexpr const char* kDefaultEndpoint = "http://192.168.11.6:1234/v1/chat/completions";
    static constexpr const char* kDefaultModel = "qwen3.5-9b-vlm";

    // エンドポイント、モデル名、APIキーを設定（空欄の場合はデフォルト/キーなし）
    static void configure(std::string endpoint, std::string model, std::string api_key);

    // 現在のカメラフレームを取得し、ローカルVLMに送信して指定方向の通行可否とスコアを評価
    static VlmEvaluation evaluate_current_view(const char* direction_label = "front");

    // VLMサーバーの現在の利用可能性ステータスを取得
    static VlmStatus get_status();

    // VLMサーバーへの軽量接続確認（ヘルスチェック）を非同期タスクでトリガー
    static void trigger_health_check(bool force_recheck = false);
};

} // namespace stackchan::app
