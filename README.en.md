[日本語](README.md)

# stackchan-idf

Firmware for M5Stack CoreS3 + Stack-chan base, written against ESP-IDF 5.4 / C++20.
Supports AI voice conversation (OpenAI / Gemini / XiaoZhi), Wi-Fi / BLE configuration, and OTA updates.

## Web Flasher / Settings page

Released firmware can be flashed straight from the browser (Chrome / Edge):

- **Flash**: <https://ciniml.github.io/stackchan-idf/>
- **BLE Settings**: <https://ciniml.github.io/stackchan-idf/settings.html>

Push a tag `vX.Y.Z` and CI builds the firmware, attaches it to a Release,
and the GitHub Pages site picks it up in its version dropdown.

Settings can be changed over **BLE** (the `settings.html` page above, encrypted)
or, once Wi-Fi is up, from the **device's built-in web page**
(`http://stackchan-XXXXXX.local/`, advertised over mDNS).

## Features

- **AI voice conversation**: connects over WebSocket to one of OpenAI Realtime /
  Google Gemini Live / a XiaoZhi server, streaming mic input → reply audio with
  mouth-sync. The CoreS3 is half-duplex, so the mic is muted while speaking;
  interrupt a reply (barge-in) with an LCD tap or a head touch (Si12T).
  Turns are detected by server-side VAD. On OpenAI / Gemini, tools
  (`set_expression` / `set_head_pose` / `speak_katakoto`) drive expression,
  head pose and robotic speech; XiaoZhi maps its reply emotion to an expression.
  The reply text streams into the speech balloon.
  - The **system prompt** (persona) and **extra HTTP headers** (e.g. a
    Cloudflare Access service token) are settable from the Wi-Fi config interface.
- **Avatar**: 30 fps face rendering with M5GFX — breath / saccade / blink animators and six expressions (Neutral / Happy / Sad / Angry / Doubt / Sleepy) with matching effects.
- **Servo control**: SCS0009 yaw + pitch over UART1 (1 Mbps, GPIO 6/7), including a trapezoidal-velocity `PathGenerator`. Torque is engaged only while moving, and the head is held still during audio playback to avoid power-rail interference.
- **Speaker**: Boot beep, randomised jtts babble synced to the avatar's mouth, AAC record + playback. Also supports BLE audio streaming and Wi-Fi RTP audio receive (L16 / μ-law / AAC).
- **Mic**: 2-second loopback (record-then-playback) sanity check at boot; also the conversation input.
- **On-device UI**: tap the top-right corner of the LCD for a tabbed info / settings / control / conversation screen.
- **Head petting (nadenade)**: a stroke across the head sensor (Si12T) triggers a happy head wobble.
- **Speech balloon**: Rounded white panel along the bottom of the screen with a 24 px Japanese-capable Gothic font. Long text scrolls right-to-left as a marquee, auto-hides after one full pass, and invokes an optional completion callback.
- **BLE settings service**: a custom BLE GATT service (NimBLE) advertises from boot, always on. Configure Wi-Fi, API keys, the various settings and OTA updates from `tools/settings.html` (Web Bluetooth, desktop Chrome / Edge). The link is encrypted via Bluetooth 4.2+ Just Works pairing (no bonding). Settings are persisted in NVS; applying them reboots the device. While Wi-Fi is down the avatar pins a "Wi-Fi: 切断中" balloon.
- **Wi-Fi settings service**: once Wi-Fi connects, the device serves an HTTP server (port 80) and a built-in web page (`settings_wifi.html`), advertised over mDNS (`stackchan-XXXXXX.local`). It can set the SSID, provider, API keys, XiaoZhi URL/token, system prompt, extra HTTP headers and jtts parameters, and perform OTA updates.
- **OTA updates**: dual OTA partitions; flash new firmware over BLE or Wi-Fi (HTTP), with boot verification and rollback.

## Hardware

- M5Stack CoreS3 (ESP32-S3, 8 MB Quad-SPI PSRAM, 16 MB flash)
- Stack-chan base (PY32 IO expander @ 0x6F, two SCS0009 servos)
  - Internal I²C: AXP2101 (0x34) / AW9523 (0x58) / FT6336 (0x38) — managed by M5Unified
  - PY32 pin 0: servo motor-voltage enable
  - Servo bus (SCS0009): UART1, TX GPIO 6 / RX GPIO 7, 1 Mbps, 8 N 1
    - Yaw  ID = 1, zero_pos = 460
    - Pitch ID = 2, zero_pos = 620
  - 1 step ≈ 0.3125° (`deg = (raw - zero) * 5 / 16`)
  - Head touch sensor Si12T @ 0x68 (3 zones, for nadenade / barge-in)

## Setup

With ESP-IDF 5.4 installed (tested against 5.4.2):

```sh
git clone <this repo>
cd stackchan-idf
git submodule update --init --recursive
tools/apply-m5-patches.sh                    # apply the one-line M5Unified fix
make set-target                              # first time only
make build
make flash PORT=/dev/ttyACM0
make monitor PORT=/dev/ttyACM0
```

`tools/apply-m5-patches.sh` just zero-initialises a `buf` array in
`M5Unified` `RTC_PowerHub_Class::setAlarmIRQ` so it stops tripping the
GCC 14 `-Werror=maybe-uninitialized` check.

API keys for OpenAI / Gemini are not baked into the build; supply them at
runtime from the BLE / Wi-Fi settings interface (stored in NVS). A compile-time
default can be given via `sdkconfig.defaults.local` (gitignored).

## Boot sequence

1. M5 / Avatar initialised, startup arpeggio (C5–E5–G5).
2. **NVS / BLE settings service / Wi-Fi**: load settings from NVS, start the BLE settings service (always advertising). If an SSID is stored, start the Wi-Fi STA connection (non-blocking); once connected, start the mDNS + HTTP config server.
3. Mic loopback test (record 2 s, then play it back).
4. Servo power on, ping both servos.
5. Start the render task (30 fps face, core 1) and servo task (20 ms tick, core 0). If conversation is enabled, the conversation task waits for Wi-Fi and starts the AI dialogue.
6. demo_loop begins — when conversation is idle: random babble, random head pose, nadenade reactions; during a conversation the AI task drives the avatar and audio.
7. Tap the top-right corner for the on-device settings UI; tap the screen during a reply to barge in.

## Repository layout

```
.
├── components/
│   ├── avatar/              face rendering + animators
│   ├── board/               CoreS3 HW bring-up + PY32 IO expander
│   ├── scs_servo/           SCS0009 driver + PathGenerator
│   ├── jtts/                Japanese katakoto TTS (babble / speak_katakoto)
│   ├── conversation/        AI voice-conversation clients (OpenAI / Gemini / XiaoZhi)
│   ├── config_service/      BLE GATT settings service + NVS + OTA + crypto
│   ├── wifi_config_service/ Wi-Fi HTTP config server + mDNS + built-in web page
│   ├── M5GFX/               submodule (upstream)
│   ├── M5Unified/           submodule (upstream + 1 patch)
│   └── tl_expected/         tl::expected backport (submodule)
├── main/                    app_main, tasks, demo loop, conversation, audio, Wi-Fi
├── patches/                 upstream-targeted patches
├── tools/                   apply-m5-patches.sh, monitor_log.py, settings.html
├── partitions.csv           OTA layout (ota_0 / ota_1 / nvs / storage)
├── sdkconfig.defaults
└── Makefile                 thin wrapper around idf.py
```

## License

First-party sources (`components/board`, `components/scs_servo`,
`components/avatar`, `components/jtts`, `components/conversation`,
`components/config_service`, `components/wifi_config_service`, `main`, `tools`)
are released under the **Boost Software License 1.0** ([LICENSE](LICENSE)).

The submodules (`components/M5GFX`, `components/M5Unified`,
`components/tl_expected/expected`) and managed_components
(`espressif/esp_audio_codec`, `espressif/esp_websocket_client`,
`espressif/mdns`) keep their respective upstream licenses.
