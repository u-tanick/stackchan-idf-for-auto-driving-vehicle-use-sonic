<!--
SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
SPDX-License-Identifier: BSL-1.0
-->

# remote-flasher

VPN の向こうの開発機 (リモートの Claude Code / CI) から、ホスト LAN に
ぶら下がっている ESP32 (CoreS3 / AtomS3R など) を flash・モニタするための
中継ツール。

```
[ remote dev / CI ]                    [ host (LAN) ]                 [ ESP32 ]
    flash-current-build.sh    POST       Bun server                   USB-CDC
    monitor-stream.sh    ─SSE/HTTP─▶   (server/server.ts)
                                            │
                                            │ WebSocket /ws
                                            ▼
                                       browser tab (Chrome)
                                       /app/ で開いた Web UI
                                       WebSerial + esptool-js  ──── flash / monitor
```

3 つの構成要素から成る:

| 役割 | 場所 | 何をするか |
|---|---|---|
| Host server | [server/](server/) | Bun 単一ファイル。`/flash` `/reset` `/monitor` `/ws` `/app/*` を提供 |
| Browser UI | [web/](web/) | WebSerial + esptool-js で実機を直接叩く SPA。ホスト経由で配信 |
| CLI クライアント | [flash-current-build.sh](flash-current-build.sh) / [monitor-stream.sh](monitor-stream.sh) | curl で `/flash` `/monitor` を呼ぶラッパ。`idf.py flash` / `idf.py monitor` 代わり |

仕様 (WebSocket フレーム形式) は [PROTOCOL.md](PROTOCOL.md) が正本。

---

## 必要なもの

- ホスト側: **Bun 1.0+**、`curl`、`jq`
- ブラウザ側: **WebSerial 対応ブラウザ** (Chrome / Edge / Chromium 系)
  + USB-CDC で ESP32 が見える OS
- WebSerial は secure context 必須。`http://localhost` は例外で動くが、
  VPN 越しでリモートからアクセスする本番運用では HTTPS で配信すること
  (自己署名でも可)。

---

## 初回セットアップ

ホスト側で 1 度だけ:

```sh
# Web UI のバンドル (esptool-js を IIFE に固める。コミット対象外)
cd tools/remote-flasher/web
bun install
bun run build      # → esptool-bundle.js が出来る

# サーバ依存
cd ../server
bun install
```

---

## 使い方 — 通常フロー

### 1. ホストでサーバを起動

```sh
cd tools/remote-flasher/server
bun server.ts                      # PORT=8765 がデフォルト
# 別ポート / 別 bind:
PORT=9000 HOST=0.0.0.0 bun server.ts
```

起動すると `http://<host>:8765/` でステータス HTML、`http://<host>:8765/app/`
で Web UI、`ws://<host>:8765/ws` で WebSocket が立つ。

### 2. ESP32 を繋いだマシンのブラウザで Web UI を開く

```
http://<host>:8765/app/
```

(リモートから繋ぐ場合は HTTPS 越し。VPN 内なら `http://localhost:8765/app/`
でも可)

UI の手順:
1. **[シリアル ポート選択]** — USB-CDC デバイスを選ぶ。自動で
   `ESPLoader.main()` が走り、チップ種別が判別される。
2. **[接続]** — WebSocket でホスト サーバに `hello` を送る。
3. Monitor を使う場合は **[モニタ開始]** を押しておく。
   ([web/README.md](web/README.md) 参照)

ここまで来たら、ブラウザ タブは開いたまま放置でよい。以降の操作はリモート
側から CLI で叩く。

### 3. リモートから flash

ビルドが終わったら、リモートの shell から:

```sh
# build-cores3/ の bootloader/partition-table/ota_data/app をまとめて POST
tools/remote-flasher/flash-current-build.sh \
    --url http://<host>:8765 \
    --build-dir build-cores3
```

- `--build-dir` 省略時は `build-cores3/`。AtomS3R は `--build-dir build-atoms3r`。
- `flasher_args.json` をパースしてセクション / オフセット / バイナリを
  自動収集するので、`make build` (`idf.py build`) の結果が変わっても追従する。
- `--erase` で全消去から、`--baud 921600` で速度変更。
- 環境変数 `REMOTE_FLASHER_URL` / `REMOTE_FLASHER_BAUD` でも指定可。

進捗は SSE で stderr に流れる。終了コード:

| 値 | 意味 |
|---|---|
| 0 | flash 成功 |
| 1 | usage / ローカル エラー (`jq` 無い、ファイル無い、等) |
| 2 | サーバから 2xx 以外、もしくは `done` が来なかった |
| 3 | サーバ経由で `done {success:false}` が返った (ブラウザ/esptool 失敗) |

### 4. リモートから serial monitor

`idf.py monitor` 相当:

```sh
tools/remote-flasher/monitor-stream.sh --url http://<host>:8765
```

- ブラウザ UI 側で **[モニタ開始]** が押されていないと無音 (`: monitor
  stream open` のコメントだけ出て止まる)。
- 直近 200 行はリング バッファからリプレイされる。複数の購読者を fan-out
  するので、同時に何本 tail しても OK。
- `-t` でホスト ローカル時刻を行頭に付与。
- Ctrl+C で抜ける。

### 5. リセットだけ

```sh
curl -X POST http://<host>:8765/reset
```

UI の **[リセット]** ボタンと同じ。DTR/RTS を叩いて再起動するだけ。

---

## トラブルシューティング

| 症状 | 原因 / 対処 |
|---|---|
| `flash-current-build.sh` が `HTTP 503` で落ちる | ブラウザ タブが繋がっていない (`/ws` のクライアント無し)。タブを開いて [接続] |
| `HTTP 409` | 既に他の flash が走っている。サーバ側のログで進捗確認 |
| `done` が来ずに 2 で終わる | 5 分タイムアウト or WS 切断。タブの DevTools コンソールで esptool ログを見る |
| `monitor-stream.sh` が無音のまま | ブラウザの [モニタ開始] が押されていないか、WebSerial がそもそも掴めていない |
| 起動直後の文字化け | ブラウザが flash 用に 460800 で開いたまま。UI の [リセット] で 115200 に戻る |

詳細は各サブディレクトリの README:

- ホスト サーバの環境変数・curl 直叩き例 → [server/README.md](server/README.md)
- ブラウザ側の操作・ビルド・ブラウザ要件 → [web/README.md](web/README.md)
- WebSocket フレームの厳密仕様 (実装者向け) → [PROTOCOL.md](PROTOCOL.md)

---

## 制約 / 既知

- ブラウザ クライアントは同時 1 つだけ。後勝ち (`superseded` を送って前を切る)。
- flash は排他 1 並列。
- WebSerial が secure context (HTTPS or localhost) 必須。
- ESP 側のチップ判別は esptool-js に任せている。CoreS3 (ESP32-S3) と
  AtomS3R (ESP32-S3) は同じパスで動く前提。
