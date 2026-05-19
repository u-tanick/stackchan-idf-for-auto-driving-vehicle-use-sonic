// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "audio_stream_sink.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_aac_dec.h>

#include "config_service/config_service.hpp"

namespace stackchan::app::audio_stream {

namespace {

constexpr const char* kTag = "audio-stream";

// AAC ADTS bytes inbound from BLE land here. 32 KiB is generous given the
// browser only fires ~20–60 chunks/sec at this BLE throughput; PSRAM-backed
// (xStreamBufferCreateWithCaps below) so it never touches internal heap.
constexpr std::size_t kStreamBufferBytes = 32 * 1024;
constexpr std::size_t kStreamBufferTrigger = 1024;

// Speaker channel reserved for BLE audio playback so it never collides
// with the conversation task's channel (0).
constexpr std::uint8_t kSpeakerChannel = 2;

// Effective BLE throughput on this build sits around 5 KiB/s. AAC-LC at
// 96 kbps is ~12 KiB/s, so we can't stream in real time. Instead we
// receive + decode into a PSRAM-resident PCM accumulator and start
// playback once the browser signals `end`. The user pays an upload-time
// latency proportional to file size, but playback itself is smooth.
constexpr std::size_t kPcmInitialCapSamples = 48000 * 4;       // 4 s @ 48 k
constexpr std::size_t kPcmGrowChunkSamples = 48000 * 4;        // grow by 4 s
constexpr std::size_t kPcmMaxCapSamples = 48000 * 5 * 60;      // hard cap: 5 min @ 48 k mono

// Playback chunking — feed the speaker in 1024-sample slices so the
// queue stays deep enough to absorb jitter but mouth_open updates at
// ~21 ms granularity (visually plenty fast).
constexpr std::size_t kPlaybackChunkSamples = 1024;

// State accessed from the GATT host task (callbacks) and the worker task.
SharedState* g_state = nullptr;
StreamBufferHandle_t g_stream = nullptr;
TaskHandle_t g_worker = nullptr;

std::atomic<bool> g_end_requested{false};
std::atomic<bool> g_abort_requested{false};
// Distinguishes a browser-initiated abort (drop everything) from a BLE
// disconnect (try to play whatever PCM we've accumulated so far).
std::atomic<bool> g_user_aborted{false};
// Drives the worker state machine. Set true by on_begin (receiving phase
// starts), cleared by the worker after playback finishes.
std::atomic<bool> g_active{false};

// --- Callbacks (run on the NimBLE host task — keep them non-blocking) ---

void on_begin(std::uint32_t sample_rate, std::uint8_t channels)
{
    g_end_requested.store(false, std::memory_order_release);
    g_abort_requested.store(false, std::memory_order_release);
    g_user_aborted.store(false, std::memory_order_release);
    g_active.store(true, std::memory_order_release);
    if (g_stream != nullptr) {
        xStreamBufferReset(g_stream);
    }
    // Tell the conv-task to yield NOW so it stops streaming mic audio over
    // the Wi-Fi WebSocket. ESP32-S3 BLE + Wi-Fi share the radio: pumping
    // ~12 KiB/s of AAC inbound on BLE leaves nothing for the conv-task's
    // outbound audio frames, and its tx queue overflows in seconds. The
    // I2S handoff itself is still cooperative (conv-task does the actual
    // mic.end / speaker.end on its own thread); we just kick the flag
    // early so radio contention drops immediately.
    if (g_state != nullptr) {
        g_state->audio_stream_active.store(true, std::memory_order_release);
    }
    if (g_worker != nullptr) {
        xTaskNotifyGive(g_worker);
    }
    ESP_LOGI(kTag, "begin: sr=%u ch=%u", static_cast<unsigned>(sample_rate),
             static_cast<unsigned>(channels));
}

void on_data(const std::uint8_t* data, std::size_t bytes)
{
    if (g_stream == nullptr || !g_active.load(std::memory_order_acquire)) return;
    // Blocking send with a short timeout so a temporary worker stall
    // doesn't drop frames — losing ADTS sync mid-file means audible
    // glitches even after the decoder resyncs.
    xStreamBufferSend(g_stream, data, bytes, pdMS_TO_TICKS(200));
}

void on_end()
{
    g_end_requested.store(true, std::memory_order_release);
    if (g_worker != nullptr) xTaskNotifyGive(g_worker);
}

void on_abort(bool user_initiated)
{
    g_abort_requested.store(true, std::memory_order_release);
    if (user_initiated) {
        g_user_aborted.store(true, std::memory_order_release);
    }
    if (g_stream != nullptr) xStreamBufferReset(g_stream);
    if (g_worker != nullptr) xTaskNotifyGive(g_worker);
}

const config::AudioStreamSink kSink{
    .on_begin = &on_begin,
    .on_data = &on_data,
    .on_end = &on_end,
    .on_abort = &on_abort,
};

// --- Worker ------------------------------------------------------------

// PSRAM-resident PCM accumulator. Decoder output goes here during the
// receive phase; playback drains it sequentially.
struct PcmBuffer {
    std::int16_t* data = nullptr;
    std::size_t samples = 0;     // valid sample count (mono frames)
    std::size_t capacity = 0;    // allocated sample count
    std::uint32_t sample_rate = 0;
    std::uint8_t channels = 0;
};

bool pcm_reserve(PcmBuffer& q, std::size_t needed_samples)
{
    if (needed_samples <= q.capacity) return true;
    if (needed_samples > kPcmMaxCapSamples) {
        ESP_LOGE(kTag, "PCM cap exhausted (%u samples requested, max %u)",
                 static_cast<unsigned>(needed_samples),
                 static_cast<unsigned>(kPcmMaxCapSamples));
        return false;
    }
    const std::size_t new_cap = std::max(q.capacity + kPcmGrowChunkSamples, needed_samples);
    auto* p = static_cast<std::int16_t*>(
        heap_caps_realloc(q.data, new_cap * sizeof(std::int16_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (p == nullptr) {
        ESP_LOGE(kTag, "pcm realloc to %u samples failed", static_cast<unsigned>(new_cap));
        return false;
    }
    q.data = p;
    q.capacity = new_cap;
    return true;
}

void pcm_release(PcmBuffer& q)
{
    if (q.data != nullptr) {
        heap_caps_free(q.data);
        q.data = nullptr;
    }
    q.samples = 0;
    q.capacity = 0;
    q.sample_rate = 0;
    q.channels = 0;
}

// Drive mouth_open from the chunk peak. Same shaping the conv-task uses.
void update_mouth(const std::int16_t* samples, std::size_t n)
{
    if (g_state == nullptr || n == 0) return;
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const std::int32_t v = std::abs(static_cast<std::int32_t>(samples[i]));
        if (v > peak) peak = v;
    }
    const float open = std::min(1.0f, static_cast<float>(peak) / 32767.0f * 1.5f);
    g_state->mouth_open.store(open, std::memory_order_relaxed);
}

void worker_task(void* /*arg*/)
{
    void* decoder = nullptr;
    auto close_decoder = [&]() {
        if (decoder != nullptr) {
            esp_aac_dec_close(decoder);
            decoder = nullptr;
        }
    };

    // Encoded-byte scratch: large enough to hold a few ADTS frames so the
    // decoder always sees complete frames even when stream-buffer reads
    // straddle a boundary.
    constexpr std::size_t kRawCap = 4096;
    auto* raw_buf = static_cast<std::uint8_t*>(
        heap_caps_malloc(kRawCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    std::size_t raw_len = 0;

    // Decoded-frame scratch: AAC frame = 1024 samples × 4 bytes (worst
    // case stereo int16). Grows on demand if a stream wants more.
    std::size_t pcm_frame_cap = 1024 * 2 * sizeof(std::int16_t) * 2;
    auto* pcm_frame = static_cast<std::uint8_t*>(
        heap_caps_malloc(pcm_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    if (raw_buf == nullptr || pcm_frame == nullptr) {
        ESP_LOGE(kTag, "scratch alloc failed (raw=%p pcm=%p)", raw_buf, pcm_frame);
        vTaskDelete(nullptr);
        return;
    }

    PcmBuffer pcm{};

    auto teardown_session = [&]() {
        M5.Speaker.stop(kSpeakerChannel);
        // Always release the I2S handoff flag in case we abort mid-playback;
        // the conv-task is waiting on it to reclaim mic + speaker.
        if (g_state != nullptr && g_state->audio_stream_active.load(std::memory_order_acquire)) {
            M5.Speaker.end();
            g_state->audio_stream_active.store(false, std::memory_order_release);
        }
        close_decoder();
        raw_len = 0;
        pcm_release(pcm);
        if (g_state != nullptr) {
            g_state->mouth_open.store(0.0f, std::memory_order_relaxed);
        }
        g_end_requested.store(false, std::memory_order_release);
        g_active.store(false, std::memory_order_release);
    };

    for (;;) {
        if (!g_active.load(std::memory_order_acquire)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }

        // ---- Phase 1: receive + decode into the PSRAM PCM buffer ----
        {
            esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
            cfg.no_adts_header = false;
            // Enable AAC-Plus (HE-AAC v1 SBR / v2 PS). Slightly larger
            // code footprint but lets ffmpeg's `aac_he` / `aac_he_v2`
            // output and any HE-encoded source files decode without
            // re-transcoding to AAC-LC first. Plain AAC-LC streams still
            // decode fine via this path.
            cfg.aac_plus_enable = true;
            if (esp_aac_dec_open(&cfg, sizeof(cfg), &decoder) != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(kTag, "esp_aac_dec_open failed");
                teardown_session();
                continue;
            }
            // Heap snapshot right after open(); compared with the post-Wi-Fi
            // failure case this tells us whether the decoder is starving on
            // internal-RAM allocations.
            ESP_LOGI(kTag,
                     "receive phase: decoder opened  INT free=%u largest=%u DMA-largest=%u  PSRAM free=%u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            raw_len = 0;
        }

        bool first_decode_error_reported = false;
        bool aborted = false;
        while (g_active.load(std::memory_order_acquire)) {
            if (g_abort_requested.exchange(false, std::memory_order_acq_rel)) {
                ESP_LOGI(kTag, "abort during receive");
                aborted = true;
                break;
            }

            // Pull more encoded bytes.
            if (raw_len < kRawCap) {
                const std::size_t got = xStreamBufferReceive(
                    g_stream, raw_buf + raw_len, kRawCap - raw_len, pdMS_TO_TICKS(100));
                raw_len += got;
            }

            // One-shot dump of the first ~64 bytes the decoder is about
            // to see. ADTS frames must start with sync 0xFFF (0xFF + low
            // nibble 0xF in the next byte) — anything else means the
            // bytes were mangled in transit (encryption, chunking, etc.)
            // rather than the decoder being at fault.
            static bool dumped = false;
            if (!dumped && raw_len >= 16) {
                char hex[3 * 32 + 1];
                char* p = hex;
                const std::size_t n = std::min<std::size_t>(raw_len, 32);
                for (std::size_t i = 0; i < n; ++i) {
                    p += std::snprintf(p, sizeof(hex) - (p - hex), "%02x ", raw_buf[i]);
                }
                ESP_LOGI(kTag, "first %u bytes to decoder: %s",
                         static_cast<unsigned>(n), hex);
                dumped = true;
            }

            // Wait until raw_buf has enough bytes to plausibly hold a
            // full ADTS frame. At 96 kbps mono 48 kHz, one frame is ~256
            // bytes; require 768 B so the decoder rarely chokes on a
            // truncated tail (which gets misinterpreted as "lost sync"
            // and we'd start dropping bytes). When end_requested is set
            // we relax this so the very last incomplete tail is at
            // least attempted.
            const bool end_pending = g_end_requested.load(std::memory_order_acquire);
            if (!end_pending && raw_len < 768) {
                continue;
            }

            // Drain decoder into PCM accumulator.
            esp_audio_dec_in_raw_t in{};
            in.buffer = raw_buf;
            in.len = raw_len;
            bool progressed = false;
            int loop_guard = 0;

            while (in.len > 0) {
                // Yield to IDLE every N iterations so the task watchdog
                // doesn't fire on long stretches of back-to-back decodes
                // (esp_aac_dec_decode is fast enough to spin for ~5 s
                // through a few KB of input without us giving up the CPU).
                if (++loop_guard % 64 == 0) {
                    vTaskDelay(1);
                }
                esp_audio_dec_out_frame_t out{};
                out.buffer = pcm_frame;
                out.len = pcm_frame_cap;
                esp_audio_dec_info_t info{};
                const auto rc = esp_aac_dec_decode(decoder, &in, &out, &info);

                if (rc == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    auto* bigger = static_cast<std::uint8_t*>(
                        heap_caps_realloc(pcm_frame, out.needed_size,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                    if (bigger == nullptr) break;
                    pcm_frame = bigger;
                    pcm_frame_cap = out.needed_size;
                    continue;
                }
                if (rc != ESP_AUDIO_ERR_OK) {
                    if (!first_decode_error_reported) {
                        ESP_LOGW(kTag,
                                 "decode rc=%d at first error  INT free=%u largest=%u DMA-largest=%u  PSRAM free=%u",
                                 static_cast<int>(rc),
                                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                        first_decode_error_reported = true;
                    }
                    // If the decoder reports failure without consuming
                    // anything AND the input buffer is mostly empty, the
                    // most likely cause is "incomplete ADTS frame at the
                    // tail" — break out and let the outer loop refill
                    // before trying again. Only drop bytes when we have
                    // enough data that the failure can't be "more data
                    // needed".
                    if (in.consumed == 0) {
                        if (!end_pending && in.len < 512) break;
                        in.buffer += 1; in.len -= 1;
                    } else {
                        in.buffer += in.consumed;
                        in.len -= in.consumed;
                    }
                    continue;
                }

                in.buffer += in.consumed;
                in.len -= in.consumed;
                progressed = true;

                if (out.decoded_size == 0) continue;

                const auto* samples = reinterpret_cast<const std::int16_t*>(out.buffer);
                const std::uint8_t ch = info.channel ? info.channel : 1;
                const std::size_t frames = out.decoded_size / sizeof(std::int16_t) / ch;

                if (pcm.sample_rate == 0) {
                    pcm.sample_rate = info.sample_rate;
                    pcm.channels = 1; // we always store mono — see below
                    if (!pcm_reserve(pcm, kPcmInitialCapSamples)) break;
                    ESP_LOGI(kTag, "format: %u Hz, %u ch source, %u kbps",
                             static_cast<unsigned>(info.sample_rate),
                             static_cast<unsigned>(ch),
                             static_cast<unsigned>(info.bitrate / 1000));
                }

                // Append mono mix-down (L channel only if source is stereo).
                if (!pcm_reserve(pcm, pcm.samples + frames)) { aborted = true; break; }
                if (ch == 1) {
                    std::memcpy(pcm.data + pcm.samples, samples, frames * sizeof(std::int16_t));
                } else {
                    for (std::size_t i = 0; i < frames; ++i) {
                        pcm.data[pcm.samples + i] = samples[i * ch];
                    }
                }
                pcm.samples += frames;
            }

            // Slide the unconsumed tail back to the front.
            if (in.len > 0 && in.buffer != raw_buf) {
                std::memmove(raw_buf, in.buffer, in.len);
            }
            raw_len = in.len;

            if (!progressed && raw_len > kRawCap / 2) {
                // Probably stale bytes — drop some to attempt resync.
                std::memmove(raw_buf, raw_buf + 256, raw_len - 256);
                raw_len -= 256;
            }

            // Done receiving when the browser signalled `end` AND the
            // pipe is fully drained.
            if (g_end_requested.load(std::memory_order_acquire) && raw_len == 0 &&
                xStreamBufferIsEmpty(g_stream)) {
                break;
            }
        }
        close_decoder();

        if (pcm.samples == 0) {
            ESP_LOGI(kTag, "receive phase ended with no PCM (aborted=%d)", aborted);
            teardown_session();
            continue;
        }
        if (aborted && g_user_aborted.load(std::memory_order_acquire)) {
            // Browser explicitly asked to abort — discard partial PCM.
            ESP_LOGI(kTag, "user-aborted; discarding %u samples",
                     static_cast<unsigned>(pcm.samples));
            teardown_session();
            continue;
        }
        if (aborted) {
            ESP_LOGW(kTag, "receive aborted by BLE disconnect; playing partial %u samples",
                     static_cast<unsigned>(pcm.samples));
            // Reset the request flag so Phase 2 doesn't bail out
            // immediately. Real user aborts go through the branch above.
            g_abort_requested.store(false, std::memory_order_release);
        }

        // ---- Phase 2: smooth playback from the PSRAM PCM buffer ----
        ESP_LOGI(kTag, "playback phase: %u samples @ %u Hz (%.2f s)",
                 static_cast<unsigned>(pcm.samples),
                 static_cast<unsigned>(pcm.sample_rate),
                 static_cast<float>(pcm.samples) / static_cast<float>(pcm.sample_rate));

        // I2S handoff was kicked off at on_begin() so the conv-task has
        // already had ~entire upload duration to yield. Confirm here with
        // a tight 500 ms timeout — if the conv-task isn't running we
        // proceed and bring up the speaker ourselves.
        const auto wait_started = xTaskGetTickCount();
        while (!g_state->conversation_yielded_i2s.load(std::memory_order_acquire)) {
            if (xTaskGetTickCount() - wait_started > pdMS_TO_TICKS(500)) {
                ESP_LOGW(kTag, "conv-task didn't yield I2S; proceeding anyway");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        // Now safe to touch I2S — nobody else owns it.
        M5.Speaker.begin();

        std::size_t pos = 0;
        while (pos < pcm.samples) {
            if (g_abort_requested.exchange(false, std::memory_order_acq_rel)) {
                ESP_LOGI(kTag, "abort during playback");
                break;
            }
            const std::size_t n = std::min(kPlaybackChunkSamples, pcm.samples - pos);
            update_mouth(pcm.data + pos, n);

            // Wait if the speaker queue is full. M5.Speaker queues per
            // channel; cap at 2 in-flight to keep latency bounded.
            while (M5.Speaker.isPlaying(kSpeakerChannel) >= 2) {
                vTaskDelay(pdMS_TO_TICKS(5));
                if (g_abort_requested.load(std::memory_order_acquire)) break;
            }
            if (g_abort_requested.load(std::memory_order_acquire)) break;

            M5.Speaker.playRaw(pcm.data + pos, n, pcm.sample_rate,
                               /*stereo=*/false, /*repeat=*/1, kSpeakerChannel,
                               /*stop_current_sound=*/false);
            pos += n;
        }

        // Drain.
        while (M5.Speaker.isPlaying(kSpeakerChannel) > 0) {
            if (g_abort_requested.load(std::memory_order_acquire)) {
                M5.Speaker.stop(kSpeakerChannel);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        // Release the I2S bus back to the conv-task.
        M5.Speaker.end();
        g_state->audio_stream_active.store(false, std::memory_order_release);

        ESP_LOGI(kTag, "playback complete");
        teardown_session();
    }
}

} // namespace

void start(SharedState& state)
{
    ESP_LOGI(kTag, "start(): allocating stream buffer (%u B)",
             static_cast<unsigned>(kStreamBufferBytes));
    g_state = &state;

    // Stream buffer in PSRAM — the default xStreamBufferCreate falls back
    // to internal-heap-preferred pvPortMalloc which doesn't have room here
    // (largest contiguous internal block is well under 32 KiB once Wi-Fi
    // + BLE + the conv-task have settled).
    g_stream = xStreamBufferCreateWithCaps(kStreamBufferBytes, kStreamBufferTrigger,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (g_stream == nullptr) {
        ESP_LOGE(kTag, "xStreamBufferCreateWithCaps failed");
        return;
    }

    // Worker task stack in PSRAM (no flash access on this path).
    if (xTaskCreatePinnedToCoreWithCaps(worker_task, "audio-stream", 8192, nullptr,
                                         tskIDLE_PRIORITY + 4, &g_worker, 1,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate audio-stream failed");
        return;
    }

    config::set_audio_stream_sink(&kSink);
    ESP_LOGI(kTag, "BLE audio stream sink registered (sink=%p)",
             static_cast<const void*>(&kSink));
}

} // namespace stackchan::app::audio_stream
