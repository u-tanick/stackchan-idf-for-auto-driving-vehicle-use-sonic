[English](README.en.md)

# stackchan-idf-for-auto-driving-vehicle-use-sonic

M5Stack CoreS3 + Atomic Motion Base + 超音波測距センサー（Sonic Unit）を組み合わせた、**超音波自律走行に特化した車輪型スタックチャン**の ESP-IDF ファームウェアです。

本リポジトリは [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) をベースに、超音波センサーによる障害物回避自律走行に特化してフォーク・最適化したものです。

---

## 🚀 主な機能・特徴

- **超音波センサーによる自律走行（SonicOnly モード）**:
  - モード選択画面や複雑な切替を廃止し、電源起動後に画面をタップするだけで即座に自律走行を開始。
  - 前方の壁・障害物を超音波センサーで常時モニタリング。
- **IMU（6軸ジャイロ）による高精度90度旋回補正**:
  - 走行床面の摩擦やバッテリー電圧に応じた旋回角速度（deg/ms）の自動キャリブレーション。
  - 目標90度に対するアンダーシュート・オーバーシュートをミリ秒単位の微小パルス駆動で自動補正。
  - 旋回後は速やかに前進へ復帰する安定制御。
- **近接時の自動バック機能**:
  - 障害物との距離が 10 cm 未満まで接近した場合は、安全な旋回半径を確保するために自動で短時間後退（約350ms）してから旋回。
- **オフライン旋回データロギング**:
  - 各旋回の目標角度、最終到達角度、誤差、補正回数、実効角速度をメモリ上に自動記録。
  - リブート時に自動クリアされるため、長時間の走行でもメモリを圧迫しません。
- **スタックチャンらしい豊かな感情表現**:
  - 障害物発見時の困り顔（Doubt）や吹き出し・カタコト音声通知。
  - 方向転換完了時の笑顔（Happy）と発話。
  - 走行中も首振りや呼吸アニメーションを継続。
- **スリム化されたクリーンなコードベース**:
  - ESP-NOW（JoyConリモート受信）や VLM（カメラ画像認識スキャン）などの実験的コードを完全に削除し、超音波走行に特化した軽量・堅牢な構造。

---

## 🛠️ ハードウェア構成

| # | パーツ | 接続・仕様 | 備考 |
|---|---|---|---|
| 1| **M5Stack CoreS3** | ESP32-S3 / 16MB Flash / 8MB PSRAM / 320×240 タッチLCD | コントローラー本体 |
| 2| **SCS0009 サーボ ×2** | UART1 (TX GPIO 6 / RX GPIO 7, 1Mbps) | スタックチャンの首（Yaw / Pitch） |
| 3| **内蔵 IMU** | BMI270 (I2C) | 旋回角度の正確なオドメトリ |
| 4| **Atomic Motion Base** | I2C (アドレス `0x38`) / DCモーター駆動 | 走行用ベース |
| 5| **超音波測距センサー** | M5Stack Unit Sonic (Atomic Motion の Port B  に接続) | 前方障害物検知 (20〜4000mm)。Base 経由で I2C (0x38) 経由にて取得 |

※ 1～3はM5StackChanのハードウェア構成に相当します

---

## 🔄 自律走行ステートマシン

```
 [起動] ──> InitWait ──> Standby (画面タップ待ち)
                            │ (画面タップ)
                            ▼
                        StartWait (1.5秒カウントダウン)
                            │
                            ▼
     ┌────────────────>  Forward (前進走行)
     │                      │
     │                      ▼ (距離 <= 200mm)
     │                 ObstacleDetected (停止 & 表情変化)
     │                      │
     │                      ▼
     │                 ObstacleDelay (1.0秒完全停止)
     │                      │
     │         ┌────────────┴────────────┐
     │         ▼ (距離 < 100mm)           ▼ (距離 >= 100mm)
     │     BackingUp (350ms後退)         │
     │         └────────────┬────────────┘
     │                      ▼
     │                   Turning (IMU 90°旋回 & 微補正)
     │                      │
     │                      ▼
     │               VerifySonicOnly (前方400msクリア確認)
     │                      │
     └──────────────────────┘ (前方クリアなら前進再開)
```

- **一時停止**: 走行中に画面をタップすると `Standby` 状態に戻り、車両が安全に停止します。

---

## 💻 ビルド・書き込み手順

ESP-IDF 5.5（本環境は 5.5.4 / 5.5.5 で動作検証済み）を使用します。

### 1. リポジトリのクローンとサブモジュール初期化

```bash
git clone https://github.com/u-tanick/stackchan-idf-for-auto-driving-vehicle-use-sonic.git
cd stackchan-idf-for-auto-driving-vehicle-use-sonic
git submodule update --init --recursive
```

### 2. 環境変数の設定とビルド (Windows PowerShell 例)

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
. C:\esp\v5.5.4\esp-idf\export.ps1

idf.py build
```

### 3. ファームウェアの書き込み & ログ確認

```powershell
idf.py -p COMx flash monitor
```
*(※ `COMx` はお使いの CoreS3 のシリアルポート番号に置き換えてください。Ctrl+] でモニターを終了できます)*

---

## 📚 ベースリポジトリ (stackchan-idf) の共通機能について

本プロジェクトは [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) の先進的なアバター制御・通信スタックを基盤としています。

以下の共通機能や詳細仕様については、退避保存されている [UPSTREAM_README.md](docs/UPSTREAM_README.md)（および [UPSTREAM_README.en.md](docs/UPSTREAM_README.en.md)）をご参照ください。

- **AI 音声対話**: OpenAI Realtime / Google Gemini Live / XiaoZhi (WebSocket) 連動
- **アバター描画 & Avatar DSL**: M5GFX による 30fps アニメーションと顔レイアウト DSL
- **オーディオ / リップシンク**: FFT ベースのマイク連動口パク、jtts によるカタコト発話
- **Web 設定 / プロビジョニング**: Web Bluetooth (BLE)、Wi-Fi HTTP (mDNS)、SoftAP キャプティブポータル
- **二重化 OTA 更新**: Web UI からのファームウェア更新

---

## 📄 ライセンス

本リポジトリのソースコードは、フォーク元に準拠し **Boost Software License 1.0** ([LICENSE](LICENSE)) の下で配布されます。

第三者コンポーネントおよび音声データの帰属表示については [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) をご覧ください。
