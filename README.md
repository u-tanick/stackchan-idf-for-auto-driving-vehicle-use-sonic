[English](README.en.md)

# stackchan-idf

M5Stack CoreS3 + Stack-chan ベース用のファームウェアです。ESP-IDF 5.4 / C++20 で書かれています。
AI 音声対話 (OpenAI / Gemini / XiaoZhi)、Wi-Fi / BLE 設定、OTA 更新に対応します。

## Web Flasher / 設定ページ

GitHub Releases に置かれたファームウェアをブラウザから書き込めます (Chrome / Edge):

- **書き込み**: <https://ciniml.github.io/stackchan-idf/>
- **BLE 設定**: <https://ciniml.github.io/stackchan-idf/settings.html>

タグ `vX.Y.Z` を push すると CI がビルドして Release を作り、Pages サイトの
バージョン一覧にも自動で追加されます。

設定は **BLE 経由** (上記 `settings.html`、暗号化) のほか、Wi-Fi 接続後は
**デバイス内蔵の Web 設定ページ** (`http://stackchan-XXXXXX.local/`、mDNS で広告)
からも行えます。

## 機能

- **AI 音声対話**: OpenAI Realtime / Google Gemini Live / XiaoZhi サーバーの
  いずれかに WebSocket で接続し、マイク入力 → 応答音声 → 口パク連動で会話。
  CoreS3 は半二重なので発話中はマイクを止め、応答中は LCD タップまたは頭タッチ
  (Si12T) で割り込み (barge-in) できる。サーバー側 VAD でターンを検出。
  OpenAI / Gemini ではツール (`set_expression` / `set_head_pose` /
  `speak_katakoto`) で表情・首振り・カタコト声を制御。XiaoZhi は応答内の感情を
  表情に反映。応答テキストは吹き出しに進行表示。
  - **システムプロンプト** (人格) と **追加 HTTP ヘッダ**
    (Cloudflare Access のサービストークン等) を Wi-Fi 設定 IF から変更可能。
- **Avatar (顔描画)**: M5GFX で 30 fps 描画。呼吸 / saccade (眼球サッカード) / blink、6 表情 (Neutral / Happy / Sad / Angry / Doubt / Sleepy) と表情に応じたエフェクト。
- **サーボ制御**: SCS0009 (Yaw / Pitch) を UART1 (1 Mbps, GPIO 6/7) で制御。台形速度プロファイルの `PathGenerator` を含む。駆動時のみトルクを有効化し、音声再生中はサーボを保持して電源干渉によるノイズを回避。
- **スピーカー**: 起動音、jtts によるランダム babble (口パク連動)、AAC 録音再生。
  BLE 経由のオーディオ ストリーム再生、Wi-Fi 経由の RTP 音声受信 (L16 / μ-law / AAC) にも対応。
- **マイク**: 起動時に 2 秒録音 → ループバック再生で動作確認。会話時の入力にも使用。
- **オンデバイス UI**: 画面右上タップで情報 / 設定 / 操作 / 会話状況のタブ式画面を表示。
- **なでなで**: 頭センサー (Si12T) のストロークで嬉しがる頭振り。
- **吹き出し (Balloon)**: 画面下部に角丸の白パネル + 24 px の日本語ゴシック フォント。長文は marquee スクロール、表示完了で自動消去し、登録したコールバックを呼び出す。
- **BLE 設定サービス**: 独自の BLE GATT サービス (NimBLE) を起動時から常時 advertising。`tools/settings.html` (Web Bluetooth、Chrome / Edge デスクトップ) から Wi-Fi 接続先・API キー・各種設定・OTA 更新を行う。Bluetooth 4.2 以降の Just Works ペアリングで暗号化通信路を確立 (ボンディング無し)。設定は NVS に保存され、適用するとデバイスが再起動して反映。Wi-Fi 切断中は balloon に「Wi-Fi: 切断中」を表示。
- **Wi-Fi 設定サービス**: Wi-Fi 接続後、デバイスが HTTP サーバー (ポート 80) と
  内蔵 Web 設定ページ (`settings_wifi.html`) を提供し、mDNS
  (`stackchan-XXXXXX.local`) で広告。SSID・プロバイダ・API キー・XiaoZhi URL/
  トークン・システムプロンプト・追加 HTTP ヘッダ・jtts 設定を変更でき、OTA 更新も可能。
- **OTA 更新**: デュアル OTA パーティション構成。BLE または Wi-Fi (HTTP) 経由で
  新ファームウェアを書き込み、起動検証付きでロールバック対応。

## ハードウェア

- M5Stack CoreS3 (ESP32-S3、8 MB Quad-SPI PSRAM、16 MB Flash)
- Stack-chan ベース (PY32 IO Expander @ 0x6F、SCS0009 ×2)
  - 内部 I2C: AXP2101 (0x34) / AW9523 (0x58) / FT6336 (0x38) ほか M5Unified が管理
  - PY32 Pin 0: サーボ VM 電源 EN
  - サーボ バス (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)
  - 頭タッチ センサー Si12T @ 0x68 (なでなで / barge-in 用、3 ゾーン)

## セットアップ

ESP-IDF 5.4 (本リポジトリは 5.4.2 で検証) を導入済みの環境で:

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # M5Unified の 1 行修正を適用
make set-target                              # 初回のみ
make build
make flash PORT=/dev/ttyACM0                 # 書き込み
make monitor PORT=/dev/ttyACM0               # シリアル モニタ
```

`tools/apply-m5-patches.sh` は upstream M5Unified の `RTC_PowerHub_Class::setAlarmIRQ` で
GCC 14 の `-Werror=maybe-uninitialized` に引っかかる `buf` 初期化を当てるだけです。

OpenAI / Gemini の API キーはビルドに埋め込まず、BLE / Wi-Fi 設定 IF から
実行時に投入します (NVS に保存)。`sdkconfig.defaults.local` (gitignore 済み) で
コンパイル時デフォルトを与えることもできます。

## 起動シーケンス

1. M5 / Avatar 初期化、起動音 (C5–E5–G5 アルペジオ)
2. **NVS / BLE 設定サービス / Wi-Fi**: NVS から設定を読み込み、BLE 設定サービスを起動 (常時 advertising)。NVS に SSID があれば Wi-Fi STA 接続を開始 (非ブロッキング)。接続後 mDNS + HTTP 設定サーバーを起動。
3. マイク loopback (2 秒録音 → 再生)
4. サーボ電源 ON → ping (Yaw / Pitch)
5. Render Task (顔描画 30 fps, core 1) + Servo Task (20 ms 周期, core 0) を起動。会話が有効なら Conversation Task が Wi-Fi 接続を待って AI 対話を開始。
6. demo_loop 開始 — 会話アイドル時はランダム babble + ランダム ポーズ + なでなで反応、会話中は AI 対話タスクがアバターと音声を制御。
7. 画面右上タップでオンデバイス設定 UI、応答中の画面タップで barge-in。

## リポジトリ構成

```
.
├── components/
│   ├── avatar/             顔描画 + アニメーション
│   ├── board/              CoreS3 HW 初期化と PY32 IO Expander
│   ├── scs_servo/          SCS0009 ドライバ + PathGenerator
│   ├── jtts/               日本語カタコト TTS (babble / speak_katakoto)
│   ├── conversation/       AI 音声対話クライアント (OpenAI / Gemini / XiaoZhi)
│   ├── config_service/     BLE GATT 設定サービス + NVS + OTA + 暗号
│   ├── wifi_config_service/ Wi-Fi HTTP 設定サーバー + mDNS + 内蔵 Web ページ
│   ├── M5GFX/              submodule (upstream)
│   ├── M5Unified/          submodule (upstream + 1 patch)
│   └── tl_expected/        tl::expected backport (submodule)
├── main/                   app_main, 各タスク, demo_loop, 会話, 音声, Wi-Fi
├── patches/                upstream 修正パッチ
├── tools/                  apply-m5-patches.sh, monitor_log.py, settings.html
├── partitions.csv          OTA 配置 (ota_0 / ota_1 / nvs / storage)
├── sdkconfig.defaults
└── Makefile                idf.py の薄いラッパ
```

## ライセンス

このリポジトリの自前ソース (`components/board`, `components/scs_servo`, `components/avatar`,
`components/jtts`, `components/conversation`, `components/config_service`,
`components/wifi_config_service`, `main`, `tools`)
は **Boost Software License 1.0** ([LICENSE](LICENSE)) の下で配布されます。

Submodule (`components/M5GFX` / `components/M5Unified` / `components/tl_expected/expected`)
と managed_components (`espressif/esp_audio_codec` / `espressif/esp_websocket_client` /
`espressif/mdns`) はそれぞれの upstream ライセンスに従います。
