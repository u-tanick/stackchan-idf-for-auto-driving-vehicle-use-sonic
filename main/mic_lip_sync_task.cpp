// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "mic_lip_sync_task.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <M5Unified.h>

#include <esp_dsp.h>
#include <dsps_fft2r.h>

namespace stackchan::app {

namespace {

constexpr const char* kTag = "mic-lip";

// 16 kHz mono. 256-sample chunks (= 16 ms = ~60 fps mouth updates) keep
// latency below the 30 fps render task with plenty of margin. 256-point
// radix-2 FFT runs in ~150 µs on the ESP32-S3 HiFi SIMD path so even with
// the per-bin magnitude / log / flux post-processing the per-chunk wall
// time stays well under 1 ms (the actual loop is gated by mic DMA delivery,
// which itself takes the full 16 ms).
constexpr std::uint32_t kSampleRate = 16'000;
constexpr std::size_t kChunkSamples = 256;
constexpr std::size_t kFftSize = kChunkSamples;
constexpr TickType_t kI2sSettle = pdMS_TO_TICKS(20);

// Voice-band bin range (RFC 7748-ish bucketing): 16000 Hz / 256 bins =
// 62.5 Hz per bin. Bin 5 ≈ 312 Hz (just above the typical room-rumble &
// HVAC band), bin 48 ≈ 3000 Hz (covers male F0+harmonics, female F0, the
// first two formants of vowels, and most consonants). Bins below 5 catch
// HVAC + the AXP2101 / SPI clock interference; above 48 catches the SCS
// servo PWM whine and high-frequency room hash — both excluded.
constexpr std::size_t kVoiceBinLo = 5;
constexpr std::size_t kVoiceBinHi = 48;  // inclusive

// Hann window precomputed once, then applied to each PCM frame before FFT
// to suppress spectral leakage between bins.
std::array<float, kFftSize> g_window{};

// Previous-frame magnitudes (linear, normalised by FFT size), kept for the
// spectral-flux onset detector. Zero on the very first frame.
std::array<float, kFftSize / 2> g_prev_mag{};

// FFT in/out interleaved-complex buffer: [Re_0, Im_0, Re_1, Im_1, ...].
// esp-dsp wants a single flat float* of length 2*N.
std::array<float, kFftSize * 2> g_fft_io{};

// Adaptive envelope shaping. Fast attack so consonant onsets open the
// mouth immediately; slow release so it closes gracefully between
// phonemes rather than chattering.
constexpr float kAttack = 0.55f;
constexpr float kRelease = 0.20f;

// Voice-band log-energy normalisation window, in raw dBFS (relative to
// i16 full-scale = 32768). Empirically, the CoreS3 internal mic puts a
// quiet room at ~-58 dBFS and a loud speech peak ~-25 dBFS. Linearly
// scaling that [-55, -22] range to [0, 1] picks up consonants at the
// low end without saturating on loud vowels. The user-visible "input
// gain" slider shifts the dBFS curve up (more sensitive) — equivalent
// to lowering kFloorDb.
constexpr float kFloorDb = -55.0f;
constexpr float kCeilDb  = -22.0f;

// Spectral-flux onset detector contribution. Flux is sum of positive
// frame-to-frame magnitude deltas in the voice band, normalised so a
// brisk /t/ /s/ /sh/ onset reaches ~1.0. Mixed via OR (max) with the
// envelope so quiet-but-fast onsets still pop the mouth open.
constexpr float kFluxFloor = 0.02f;
constexpr float kFluxCeil  = 0.20f;
constexpr float kFluxWeight = 0.7f;

bool g_fft_ready = false;

// One-shot FFT table init. Returns true if dsps_fft2r_init_fc32 succeeded.
bool init_fft_once()
{
    if (g_fft_ready) return true;
    if (const esp_err_t err = dsps_fft2r_init_fc32(nullptr, kFftSize); err != ESP_OK) {
        ESP_LOGE(kTag, "dsps_fft2r_init_fc32 failed: 0x%x", err);
        return false;
    }
    // Hann window: w[n] = 0.5 * (1 - cos(2π n / (N-1))).
    const float two_pi_over_nm1 = 2.0f * static_cast<float>(M_PI) /
                                  static_cast<float>(kFftSize - 1);
    for (std::size_t i = 0; i < kFftSize; ++i) {
        g_window[i] = 0.5f * (1.0f - std::cos(two_pi_over_nm1 * static_cast<float>(i)));
    }
    g_fft_ready = true;
    return true;
}

// Estimate mouth_open ∈ [0, 1] from a frame of i16 PCM.
//   1. Hann-window into the FFT complex buffer (Im = 0)
//   2. radix-2 in-place FFT (esp-dsp HiFi SIMD path)
//   3. Bit-reverse so bins are in natural order
//   4. Sum the magnitude² of bins in the voice band → log → [0, 1] env
//   5. Spectral flux: sum positive frame-to-frame magnitude deltas across
//      the same band → [0, 1] flux
//   6. Combine: result = max(env, kFluxWeight × flux). The OR keeps the
//      stationary voiced regions while letting fast consonant attacks
//      pop the mouth open.
//   7. Update g_prev_mag for next frame.
// `gain` multiplies the linear time-domain signal before windowing, so
// the user's input-gain slider shifts dBFS but doesn't risk clipping —
// FFT bin energies grow / shrink linearly with the input.
float estimate_mouth_open(const std::int16_t* buf, std::size_t n, float gain)
{
    if (n != kFftSize) return 0.0f;
    if (!init_fft_once()) return 0.0f;

    // 1. Window + cast i16 → float into the interleaved-complex buffer.
    constexpr float kI16Scale = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < kFftSize; ++i) {
        g_fft_io[i * 2 + 0] = static_cast<float>(buf[i]) * kI16Scale * gain * g_window[i];
        g_fft_io[i * 2 + 1] = 0.0f;
    }

    // 2-3. FFT (in-place) + bit-reverse.
    dsps_fft2r_fc32(g_fft_io.data(), kFftSize);
    dsps_bit_rev_fc32(g_fft_io.data(), kFftSize);

    // 4. Voice-band log-energy.
    float band_sumsq = 0.0f;
    float flux = 0.0f;
    std::array<float, kFftSize / 2> mag{};
    for (std::size_t k = kVoiceBinLo; k <= kVoiceBinHi && k < kFftSize / 2; ++k) {
        const float re = g_fft_io[k * 2 + 0];
        const float im = g_fft_io[k * 2 + 1];
        const float mag_sq = re * re + im * im;
        const float m = std::sqrt(mag_sq);
        mag[k] = m;
        band_sumsq += mag_sq;
        // 5. Spectral flux (positive deltas only).
        const float d = m - g_prev_mag[k];
        if (d > 0.0f) flux += d;
    }
    g_prev_mag = mag;

    // Convert sum of squared magnitudes → dBFS-ish. Add a tiny epsilon to
    // avoid -inf at perfect silence.
    const float band_energy = std::sqrt(band_sumsq) + 1e-9f;
    const float db = 20.0f * std::log10(band_energy);
    float env = (db - kFloorDb) / (kCeilDb - kFloorDb);
    if (env < 0.0f) env = 0.0f;
    if (env > 1.0f) env = 1.0f;

    // 6. Flux normalisation + OR mix.
    float flux_n = (flux - kFluxFloor) / (kFluxCeil - kFluxFloor);
    if (flux_n < 0.0f) flux_n = 0.0f;
    if (flux_n > 1.0f) flux_n = 1.0f;
    float combined = env;
    const float flux_contrib = kFluxWeight * flux_n;
    if (flux_contrib > combined) combined = flux_contrib;
    return combined;
}

void mic_lip_sync_task_entry(void* arg)
{
    auto* state = static_cast<SharedState*>(arg);
    if (state == nullptr) {
        ESP_LOGE(kTag, "null state, aborting task");
        vTaskDelete(nullptr);
        return;
    }

    std::array<std::int16_t, kChunkSamples> buf{};
    bool mic_owned = false;
    float smoothed = 0.0f;

    ESP_LOGI(kTag, "mic lip-sync task started (rate=%u Hz, chunk=%u samples)",
             static_cast<unsigned>(kSampleRate), static_cast<unsigned>(kChunkSamples));

    for (;;) {
        // Speaker takes precedence: if any other producer (balloon say, MCP
        // say, jtts babble-in-emergency-fallback) starts playing, release
        // the mic so the I2S bus is free for the speaker. We pick the mic
        // back up once the speaker finishes.
        if (M5.Speaker.isPlaying()) {
            if (mic_owned) {
                M5.Mic.end();
                vTaskDelay(kI2sSettle);
                mic_owned = false;
                // Decay the smoothed envelope so the mouth closes while the
                // other producer drives `mouth_open` itself.
                smoothed = 0.0f;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!mic_owned) {
            M5.Speaker.end();
            vTaskDelay(kI2sSettle);
            if (!M5.Mic.begin()) {
                // No mic on this board — log once and back off. The retry
                // delay is long enough that we don't spam the log.
                ESP_LOGW(kTag, "M5.Mic.begin failed; retrying in 5 s");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            mic_owned = true;
        }

        if (!M5.Mic.record(buf.data(), buf.size(), kSampleRate, /*stereo=*/false)) {
            // Mic transitions are racy under heavy load (e.g. someone called
            // playRaw between our isPlaying check and record). Yield briefly
            // and let the next loop iteration redo the speaker check.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Wait for the queued buffer to finish capturing. The record call
        // queues asynchronously; isRecording() returns 0 once the DMA has
        // delivered the whole chunk.
        for (int i = 0; i < 100 && M5.Mic.isRecording(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        // Pull the calibration sliders. Both are integer percent (100 = 1.0x).
        // Clamp to a sane range so a corrupted NVS slot can't divide-by-zero
        // or send the mouth to NaN.
        const std::uint16_t in_pct  = state->mic_lip_input_gain_pct.load(
            std::memory_order_relaxed);
        const std::uint16_t out_pct = state->mic_lip_output_gain_pct.load(
            std::memory_order_relaxed);
        const float in_gain  = static_cast<float>(in_pct  ? in_pct  : 100) / 100.0f;
        const float out_gain = static_cast<float>(out_pct ? out_pct : 100) / 100.0f;

        // FFT-based voice-band log energy + spectral-flux onset detector.
        // estimate_mouth_open already normalises to [0, 1]; output gain
        // scales beyond that, saturating quiet inputs to full mouth open.
        float level = estimate_mouth_open(buf.data(), buf.size(), in_gain);
        level *= out_gain;
        if (level > 1.0f) level = 1.0f;
        if (level < 0.0f) level = 0.0f;

        // Attack/release smoothing so the mouth doesn't pop between every
        // sample window. Rising edges use the faster coefficient.
        const float coef = (level > smoothed) ? kAttack : kRelease;
        smoothed += (level - smoothed) * coef;

        state->mouth_open.store(smoothed, std::memory_order_relaxed);
    }
}

} // namespace

void start_mic_lip_sync_task(SharedState& state)
{
    // 4 KB internal RAM stack — plenty for a 512-byte mic buffer + sqrt loop
    // + M5Unified call frames. Pinned to core 1 alongside the render task so
    // it shares Core 0 with the (now disabled) conversation/audio paths.
    xTaskCreatePinnedToCore(mic_lip_sync_task_entry, "mic-lip", 4096, &state,
                            tskIDLE_PRIORITY + 2, nullptr, 1);
}

} // namespace stackchan::app
