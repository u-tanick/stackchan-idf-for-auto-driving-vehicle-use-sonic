# audio-cli

BLE 経由音声ストリーミングの **ブラウザ非依存版** CLI。`tools/settings.html`
の「音声ファイル再生 (BLE ストリーミング)」と同じワイヤプロトコル(X25519 +
AES-256-GCM、AudioControl/AudioData chr、ADTS AAC ストリーミング)を Rust で
再実装。

## 用途

`tools/settings.html` から音声を送ると 40 秒前後で BLE が切断される問題
(`reason=531`、Chrome 側が `HCI 0x13 Remote User Terminated Connection` を
送ってくる)が、Chrome / OS BLE スタック起因なのかデバイス側起因なのかを
切り分けるためのもの。

CLI で同じファイルを送って:

- **完走するなら** → Chrome の write キュー挙動が原因。設定ツール側で
  さらに pacing を絞る等の対応が要る
- **やはり ~40 秒で切れるなら** → デバイス側 BLE スタック (NimBLE) ないし
  両端 LL レイヤ起因。NimBLE のチューニングが必要

## ビルド

```sh
cd tools/audio-cli
cargo build --release
```

Linux の場合、Bluetooth ヘッダ + DBus が必要(`libbluetooth-dev libdbus-1-dev`)。
macOS は CoreBluetooth、Windows は WinRT を btleplug がそのまま使う。

## 入力フォーマット

ADTS フレーム形式の AAC ファイルを期待します。ffmpeg で任意ソースから変換:

```sh
ffmpeg -i input.mp3 -c:a aac -b:a 96k -ar 48000 -ac 1 -f adts out.aac
```

`-f adts` を指定しないと MP4 コンテナになって ADTS sync が無く再生できません。

## 使い方

```sh
./target/release/audio-cli --device Stackchan-E2604E --file out.aac

# 詳細指定
./target/release/audio-cli \
    --device Stackchan- \
    --file out.aac \
    --chunk-size 250 \
    --rate-kbps 6 \
    --flush-every 16
```

## オプション

| フラグ | 既定 | 説明 |
|---|---|---|
| `--device <prefix>` | `Stackchan-` | BLE local name の prefix。最初にマッチしたものに繋ぐ |
| `--file <path>` | (必須) | ADTS AAC ファイル |
| `--chunk-size <N>` | 250 | 1 BLE write あたりの平文サイズ。暗号化で +28B、上限 512B |
| `--rate-kbps <K>` | 6.0 | 目標スループット。BLE 実効よりちょい遅めに |
| `--flush-every <N>` | 16 | N 回に 1 回 with-response。1 なら毎回 with-response (遅いが安定) |
| `--sample-rate <Hz>` | 48000 | begin に書く sample_rate(実際は ADTS から自動検出) |
| `--handshake-only` | false | 接続 + 鍵交換だけして抜ける(リンク確認用) |

## トラブルシュート

- Linux: 動かないときは `sudo setcap 'cap_net_raw,cap_net_admin+eip' target/release/audio-cli` でケーパビリティ付与
- スキャンに 20 秒以上かかる場合 → デバイスが advertise してないか、すでに別の中央に繋がってる
