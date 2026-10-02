[日本語](README.md)

# stackchan-idf-for-auto-driving-vehicle-use-sonic

ESP-IDF firmware for a **wheeled Stack-chan specialized in ultrasonic autonomous driving**, combining M5Stack CoreS3, Atomic Motion Base, and an Ultrasonic Distance Sensor (Unit Sonic).

This repository is forked from [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf) and optimized specifically for obstacle-avoiding autonomous driving using an ultrasonic sensor.

---

## 🚀 Key Features

- **Ultrasonic Autonomous Driving (SonicOnly Mode)**:
  - Eliminates mode selection screens and complex settings. Tap the screen after boot to immediately begin autonomous driving.
  - Continuously monitors front obstacles and walls with the ultrasonic sensor.
- **High-Precision 90° Turn Correction via IMU (6-Axis Gyro)**:
  - Automatically calibrates angular turn rate (deg/ms) based on floor friction and battery voltage.
  - Automatically compensates for undershoot and overshoot relative to the target 90° turn using millisecond-level micro-pulses.
  - Promptly resumes forward motion after turning with high stability.
- **Automatic Backup on Close Proximity**:
  - If an obstacle is detected closer than 10 cm, the vehicle automatically backs up for a short duration (~350 ms) to secure clearance before turning.
- **Offline Turn Data Logging**:
  - Automatically records target angle, final reached angle, error, iteration count, and effective angular rate in memory for each turn.
  - Cleared automatically upon reboot, preventing memory exhaustion during long runs.
- **Rich Stack-chan Emotional Expressions**:
  - Troubled face (Doubt), balloon text, and katakoto robotic voice alerts upon obstacle detection.
  - Happy face (Happy) and voice announcement upon completing a turn.
  - Continuous neck gestures and breathing animations even while driving.
- **Streamlined, Clean Codebase**:
  - Experimental code such as ESP-NOW (JoyCon remote receiver) and VLM (camera vision scanning) has been completely removed for a lightweight, robust architecture.

---

## 🛠️ Hardware Requirements

| # | Component | Interface / Spec | Remarks |
|---|---|---|---|
| 1 | **M5Stack CoreS3** | ESP32-S3 / 16MB Flash / 8MB PSRAM / 320×240 Touch LCD | Main controller |
| 2 | **SCS0009 Servos ×2** | UART1 (TX GPIO 6 / RX GPIO 7, 1Mbps) | Stack-chan neck (Yaw / Pitch) |
| 3 | **Built-in IMU** | BMI270 (I2C) | Accurate odometry for turning angle |
| 4 | **Atomic Motion Base** | I2C (Address `0x38`) / DC Motor Driver | Driving base |
| 5 | **Ultrasonic Sensor** | M5Stack Unit Sonic (Connected to Atomic Motion Port B) | Front obstacle detection (20–4000mm). Read via Base over I2C (0x38) |

*Note: Items 1 to 3 correspond to the standard M5StackChan hardware configuration.*

---

## 🔄 Autonomous Driving State Machine

```
 [Boot] ──> InitWait ──> Standby (Wait for screen tap)
                            │ (Screen tap)
                            ▼
                        StartWait (1.5s countdown)
                            │
                            ▼
     ┌────────────────>  Forward (Forward driving)
     │                      │
     │                      ▼ (Distance <= 200mm)
     │                 ObstacleDetected (Stop & expression change)
     │                      │
     │                      ▼
     │                 ObstacleDelay (1.0s complete stop)
     │                      │
     │         ┌────────────┴────────────┐
     │         ▼ (Distance < 100mm)       ▼ (Distance >= 100mm)
     │     BackingUp (350ms backup)      │
     │         └────────────┬────────────┘
     │                      ▼
     │                   Turning (IMU 90° spin & fine correction)
     │                      │
     │                      ▼
     │               VerifySonicOnly (Wait 400ms to verify clear front)
     │                      │
     └──────────────────────┘ (Resume forward driving if clear)
```

- **Pause**: Tapping the screen while driving returns the state to `Standby` and safely stops the vehicle.

---

## 💻 Build and Flash Instructions

Built and tested against ESP-IDF 5.5 (5.5.4 / 5.5.5).

### 1. Clone the Repository and Initialize Submodules

```bash
git clone https://github.com/u-tanick/stackchan-idf-for-auto-driving-vehicle-use-sonic.git
cd stackchan-idf-for-auto-driving-vehicle-use-sonic
git submodule update --init --recursive
```

### 2. Environment Setup & Build (Windows PowerShell Example)

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
. C:\esp\v5.5.4\esp-idf\export.ps1

idf.py build
```

### 3. Flash Firmware & Monitor Logs

```powershell
idf.py -p COMx flash monitor
```
*(Replace `COMx` with your CoreS3 serial port. Press `Ctrl+]` to exit the monitor)*

---

## 📚 Base Repository (stackchan-idf) Features

This project is built upon the advanced avatar control and communications stack of [ciniml/stackchan-idf](https://github.com/ciniml/stackchan-idf).

For details on common features and upstream specifications, please refer to the preserved [UPSTREAM_README.en.md](docs/UPSTREAM_README.en.md) (or [UPSTREAM_README.md](docs/UPSTREAM_README.md)):

- **AI Voice Conversation**: OpenAI Realtime / Google Gemini Live / XiaoZhi (WebSocket) integration
- **Avatar Rendering & Avatar DSL**: 30 fps animations with M5GFX and live DSL bytecode updates
- **Audio / Lip-Sync**: FFT-based microphone mouth-sync and jtts speech synthesis
- **Web Configuration / Provisioning**: Web Bluetooth (BLE), Wi-Fi HTTP (mDNS), and SoftAP captive portal
- **Dual OTA Updates**: On-device firmware updates via web UI

---

## 📄 License

The source code in this repository is distributed under the **Boost Software License 1.0** ([LICENSE](LICENSE)) conforming to the upstream project.

For third-party components and audio data attribution, see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
