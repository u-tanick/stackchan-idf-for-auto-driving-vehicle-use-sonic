// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "wifi_audio.hpp"
#include "wifi_audio_depacketizer.hpp"
#include "wifi_sta.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <M5Unified.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lwip/sockets.h>

namespace stackchan::app::wifi_audio {

namespace {

constexpr const char* kTag = "wifi-audio";

// UDP port the RTP listener binds. Distinct from VBAN's 6980 to avoid
// confusing a VBAN sender into thinking we speak VBAN.
constexpr std::uint16_t kPort = 6970;

// Fixed playback format. ffmpeg/gst send `-ar 48000 -ac 1` (downmix). The
// speaker is mono; stereo would also stress the trimmed Wi-Fi RX buffers.
constexpr std::uint32_t kSampleRate = 48000;

// Speaker channel reserved for streamed audio (same as the BLE sink) so it
// never collides with the conversation task's channel 0 or the demo babble.
constexpr std::uint8_t kSpeakerChannel = 2;

// Live jitter buffer: ~100 ms pre-roll before playback starts, ~500 ms ring.
// Small on purpose — this is live audio, latency matters more than smoothing.
constexpr std::size_t kChunkSamples = 480;        // 10 ms @ 48 kHz, one playRaw chunk
constexpr std::size_t kPrerollSamples = 4800;     // 100 ms before first playRaw
constexpr std::size_t kRingSamples = 24000;       // 500 ms cap
constexpr std::size_t kScratchBuffers = 4;        // > speaker queue depth (2)

// Largest silence we'll synthesize for one gap. Beyond this we treat the
// timestamp jump as a discontinuity (stream restart) rather than filling.
constexpr std::size_t kMaxGapSamples = kSampleRate / 2; // 0.5 s

constexpr int kRecvTimeoutMs = 5;
constexpr std::uint32_t kStopSilenceMs = 500;     // no packets this long → stop
constexpr std::size_t kRecvBufBytes = 1600;       // > one UDP/RTP/L16 datagram

SharedState* g_state = nullptr;
bool g_conversation_enabled = false;

inline std::uint32_t now_ms()
{
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

// --- Mouth animation (mirrors audio_stream_sink.cpp's adaptive RMS→dB) ----

float g_mouth_smoothed = 0.0f;
float g_db_baseline = -30.0f;
bool g_db_baseline_init = false;

void update_mouth(const std::int16_t* samples, std::size_t n)
{
    if (g_state == nullptr || n == 0) return;
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = samples[i];
        sum_sq += s * s;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(n));
    const float db = (rms >= 1.0)
                         ? 20.0f * std::log10(static_cast<float>(rms) / 32768.0f)
                         : -96.0f;
    constexpr float kSilenceDb = -55.0f;
    if (db > kSilenceDb) {
        if (!g_db_baseline_init) {
            g_db_baseline = db;
            g_db_baseline_init = true;
        } else {
            g_db_baseline += 0.03f * (db - g_db_baseline);
        }
    }
    constexpr float kLowDb = 2.0f;
    constexpr float kHighDb = 8.0f;
    float target = (db - (g_db_baseline - kLowDb)) / (kLowDb + kHighDb);
    target = std::clamp(target, 0.0f, 1.0f);
    constexpr float kAttack = 0.95f;
    constexpr float kRelease = 0.65f;
    const float a = (target > g_mouth_smoothed) ? kAttack : kRelease;
    g_mouth_smoothed += a * (target - g_mouth_smoothed);
    g_state->mouth_open.store(g_mouth_smoothed, std::memory_order_relaxed);
}

// --- PCM ring (single producer = single consumer = the receiver task) ------

struct Ring {
    std::int16_t* data = nullptr;
    std::size_t capacity = 0;
    std::uint32_t write = 0;
    std::uint32_t read = 0;

    std::size_t available_read() const { return write - read; }
    std::size_t available_write() const { return capacity - available_read(); }

    void push(const std::int16_t* src, std::size_t n)
    {
        const std::size_t wpos = write % capacity;
        const std::size_t tail = std::min(n, capacity - wpos);
        std::memcpy(data + wpos, src, tail * sizeof(std::int16_t));
        if (n > tail) std::memcpy(data, src + tail, (n - tail) * sizeof(std::int16_t));
        write += n;
    }
    void push_zeros(std::size_t n)
    {
        const std::size_t wpos = write % capacity;
        const std::size_t tail = std::min(n, capacity - wpos);
        std::memset(data + wpos, 0, tail * sizeof(std::int16_t));
        if (n > tail) std::memset(data, 0, (n - tail) * sizeof(std::int16_t));
        write += n;
    }
    void pop(std::int16_t* dst, std::size_t n)
    {
        const std::size_t rpos = read % capacity;
        const std::size_t tail = std::min(n, capacity - rpos);
        std::memcpy(dst, data + rpos, tail * sizeof(std::int16_t));
        if (n > tail) std::memcpy(dst + tail, data, (n - tail) * sizeof(std::int16_t));
        read += n;
    }
};

// --- Live player: PcmSink + jitter ring + speaker feed --------------------

class LivePlayer : public PcmSink {
public:
    bool init()
    {
        ring_.data = static_cast<std::int16_t*>(
            heap_caps_malloc(kRingSamples * sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        ring_.capacity = kRingSamples;
        for (auto& s : scratch_) {
            s = static_cast<std::int16_t*>(
                heap_caps_malloc(kChunkSamples * sizeof(std::int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (s == nullptr) return false;
        }
        return ring_.data != nullptr;
    }

    void begin_session()
    {
        ring_.write = ring_.read = 0;
        playing_ = false;
        chunks_played_ = 0;
        last_diag_ms_ = now_ms();
        g_mouth_smoothed = 0.0f;
        g_db_baseline_init = false;
        if (g_state != nullptr) g_state->audio_stream_active.store(true, std::memory_order_release);
        const bool spk_ok = M5.Speaker.begin();
        ESP_LOGI(kTag, "stream begin (%u Hz mono, speaker=%d)", static_cast<unsigned>(kSampleRate),
                 spk_ok ? 1 : 0);
    }

    void end_session()
    {
        M5.Speaker.stop(kSpeakerChannel);
        ring_.write = ring_.read = 0;
        playing_ = false;
        if (g_state != nullptr) {
            g_state->mouth_open.store(0.0f, std::memory_order_relaxed);
            g_state->audio_stream_active.store(false, std::memory_order_release);
        }
        ESP_LOGI(kTag, "stream end");
    }

    // PcmSink — called from the depacketizer (same task).
    void push(const std::int16_t* samples, std::size_t n) override
    {
        if (n > ring_.capacity) { samples += (n - ring_.capacity); n = ring_.capacity; }
        if (ring_.available_write() < n) ring_.read += (n - ring_.available_write()); // drop oldest
        ring_.push(samples, n);
    }
    void push_silence(std::size_t n) override
    {
        n = std::min(n, ring_.capacity);
        if (ring_.available_write() < n) ring_.read += (n - ring_.available_write());
        ring_.push_zeros(n);
    }

    // Feed the speaker from the ring. Non-blocking: playRaw only enqueues
    // while a slot is free (queue depth 2), so this returns promptly.
    void service()
    {
        if (!playing_) {
            if (ring_.available_read() < kPrerollSamples) return;
            playing_ = true;
            ESP_LOGI(kTag, "playback started (%u samples pre-rolled)",
                     static_cast<unsigned>(ring_.available_read()));
        }
        while (M5.Speaker.isPlaying(kSpeakerChannel) < 2 && ring_.available_read() >= kChunkSamples) {
            auto* buf = scratch_[scratch_idx_];
            scratch_idx_ = (scratch_idx_ + 1) % scratch_.size();
            ring_.pop(buf, kChunkSamples);
            update_mouth(buf, kChunkSamples);
            M5.Speaker.playRaw(buf, kChunkSamples, kSampleRate, /*stereo=*/false,
                               /*repeat=*/1, kSpeakerChannel, /*stop_current_sound=*/false);
            ++chunks_played_;
        }
        // Flip CONFIG_LOG_DEFAULT_LEVEL_DEBUG to confirm the speaker is
        // draining the ring (chunks should climb at 48 kHz; ring stays bounded).
        const std::uint32_t t = now_ms();
        if (t - last_diag_ms_ > 2000) {
            last_diag_ms_ = t;
            ESP_LOGD(kTag, "play diag: chunks=%u ring=%uS spk_q=%d",
                     static_cast<unsigned>(chunks_played_),
                     static_cast<unsigned>(ring_.available_read()),
                     M5.Speaker.isPlaying(kSpeakerChannel));
        }
    }

private:
    Ring ring_{};
    std::array<std::int16_t*, kScratchBuffers> scratch_{};
    std::size_t scratch_idx_ = 0;
    bool playing_ = false;
    std::uint32_t chunks_played_ = 0;
    std::uint32_t last_diag_ms_ = 0;
};

// --- L16 (raw PCM) depacketizer -------------------------------------------

class L16Depacketizer : public IDepacketizer {
public:
    void on_packet(const std::uint8_t* payload, std::size_t len, std::uint32_t rtp_ts,
                   bool /*marker*/, std::uint32_t /*lost*/, PcmSink& out) override
    {
        // Conceal loss with silence sized from the timestamp jump (mono: the
        // RTP timestamp counts samples). Skip on overlap/dup; treat an
        // implausibly large jump as a discontinuity (no fill).
        if (have_ts_) {
            const std::int32_t gap = static_cast<std::int32_t>(rtp_ts - expected_ts_);
            if (gap > 0 && static_cast<std::size_t>(gap) <= kMaxGapSamples) {
                out.push_silence(static_cast<std::size_t>(gap));
            }
        }
        std::size_t nsamp = len / 2;
        if (nsamp > buf_.size()) nsamp = buf_.size();
        for (std::size_t i = 0; i < nsamp; ++i) {
            // RTP/L16 payload is big-endian (network order) signed 16-bit.
            buf_[i] = static_cast<std::int16_t>((payload[2 * i] << 8) | payload[2 * i + 1]);
        }
        out.push(buf_.data(), nsamp);
        expected_ts_ = rtp_ts + static_cast<std::uint32_t>(nsamp);
        have_ts_ = true;
    }
    void reset() override { have_ts_ = false; }

private:
    bool have_ts_ = false;
    std::uint32_t expected_ts_ = 0;
    std::array<std::int16_t, 1024> buf_{}; // ≥ one datagram of mono samples
};

// --- μ-law (G.711 PCMU) depacketizer --------------------------------------

// Standard G.711 μ-law → 14-bit linear (stored as int16). Used by OBS, which
// emits PCMU on RTP payload type 0. PCMU is 8 kHz narrowband, so we upsample
// to the device rate (×6 for 8 kHz → 48 kHz) with linear interpolation inside
// the depacketizer — the player / ring stay at the fixed device rate.
class MuLawDepacketizer : public IDepacketizer {
public:
    explicit MuLawDepacketizer(std::uint32_t input_rate)
        : input_rate_(input_rate ? input_rate : 8000)
    {
        ratio_ = kSampleRate / input_rate_;
        if (ratio_ == 0) ratio_ = 1; // input already ≥ device rate → no upsample
        for (int b = 0; b < 256; ++b) table_[b] = decode_one(static_cast<std::uint8_t>(b));
    }

    void on_packet(const std::uint8_t* payload, std::size_t len, std::uint32_t rtp_ts,
                   bool /*marker*/, std::uint32_t /*lost*/, PcmSink& out) override
    {
        // Gap is measured at the INPUT clock (PCMU ts counts 8 kHz samples);
        // emit silence at the OUTPUT rate (× ratio).
        if (have_ts_) {
            const std::int32_t gap = static_cast<std::int32_t>(rtp_ts - expected_ts_);
            if (gap > 0 && static_cast<std::size_t>(gap) <= kMaxGapSamples) {
                out.push_silence(static_cast<std::size_t>(gap) * ratio_);
                last_ = 0; // discontinuity — ramp the next sample up from silence
            }
        }
        std::array<std::int16_t, 1024> obuf;
        std::size_t oi = 0;
        auto flush = [&] {
            if (oi) { out.push(obuf.data(), oi); oi = 0; }
        };
        for (std::size_t i = 0; i < len; ++i) {
            const std::int32_t s = table_[payload[i]];
            // Linear interpolation last_ → s over `ratio_` output samples.
            for (std::uint32_t k = 1; k <= ratio_; ++k) {
                obuf[oi++] = static_cast<std::int16_t>(
                    last_ + (s - last_) * static_cast<std::int32_t>(k) / static_cast<std::int32_t>(ratio_));
                if (oi == obuf.size()) flush();
            }
            last_ = s;
        }
        flush();
        expected_ts_ = rtp_ts + static_cast<std::uint32_t>(len); // 1 byte = 1 input sample
        have_ts_ = true;
    }

    void reset() override
    {
        have_ts_ = false;
        last_ = 0;
    }

private:
    static std::int16_t decode_one(std::uint8_t u)
    {
        u = ~u;
        std::int32_t t = ((u & 0x0F) << 3) + 0x84;
        t <<= (static_cast<unsigned>(u) & 0x70) >> 4;
        return static_cast<std::int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }

    std::uint32_t input_rate_;
    std::uint32_t ratio_;
    std::array<std::int16_t, 256> table_{};
    bool have_ts_ = false;
    std::uint32_t expected_ts_ = 0;
    std::int32_t last_ = 0; // previous decoded sample, for interpolation continuity
};

// --- RTP parse ------------------------------------------------------------

struct RtpView {
    bool valid = false;
    std::uint8_t pt = 0;
    bool marker = false;
    std::uint16_t seq = 0;
    std::uint32_t ts = 0;
    const std::uint8_t* payload = nullptr;
    std::size_t payload_len = 0;
};

RtpView parse_rtp(const std::uint8_t* buf, std::size_t len)
{
    RtpView v;
    if (len < 12 || (buf[0] >> 6) != 2) return v; // need header + RTP version 2
    const std::uint8_t cc = buf[0] & 0x0F;
    const bool ext = (buf[0] >> 4) & 0x01;
    std::size_t hdr = 12 + static_cast<std::size_t>(cc) * 4;
    if (len < hdr) return v;
    if (ext) {
        if (len < hdr + 4) return v;
        const std::uint16_t words = (buf[hdr + 2] << 8) | buf[hdr + 3];
        hdr += 4 + static_cast<std::size_t>(words) * 4;
        if (len < hdr) return v;
    }
    v.marker = (buf[1] >> 7) & 0x01;
    v.pt = buf[1] & 0x7F;
    v.seq = (buf[2] << 8) | buf[3];
    v.ts = (static_cast<std::uint32_t>(buf[4]) << 24) | (buf[5] << 16) | (buf[6] << 8) | buf[7];
    v.payload = buf + hdr;
    v.payload_len = len - hdr;
    v.valid = true;
    return v;
}

// Map an RTP payload type to a codec + input sample rate. PT 0 is the static
// assignment for G.711 PCMU (μ-law, 8 kHz) — what OBS sends. Any other PT is
// treated as our L16/48000 (ffmpeg/gst dynamic PT). This is the standard
// PT→codec role, so senders pick the codec just by their command.
struct CodecSel {
    Codec codec;
    std::uint32_t rate;
    const char* name;
};
CodecSel codec_for_pt(std::uint8_t pt)
{
    if (pt == 0) return {Codec::MuLaw, 8000, "PCMU/8000 (G.711 mu-law)"};
    return {Codec::L16, kSampleRate, "L16/48000"};
}

// --- Receiver task --------------------------------------------------------

void receiver_task(void* /*arg*/)
{
    // Wait for the Wi-Fi link before binding.
    while (!wifi_is_connected()) vTaskDelay(pdMS_TO_TICKS(200));

    int sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(kTag, "socket() failed: errno=%d", errno);
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "bind(%u) failed: errno=%d", kPort, errno);
        ::close(sock);
        vTaskDelete(nullptr);
        return;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = kRecvTimeoutMs * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto player = std::make_unique<LivePlayer>();
    if (!player->init()) {
        ESP_LOGE(kTag, "player alloc failed");
        ::close(sock);
        vTaskDelete(nullptr);
        return;
    }
    // Codec is selected per stream from the RTP payload type (see
    // codec_for_pt). Built lazily on the first packet (and rebuilt if the PT
    // changes); AAC plugs in via make_depacketizer() later.
    std::unique_ptr<IDepacketizer> depack;
    int current_pt = -1;

    ESP_LOGI(kTag, "RTP listening on udp/%u (L16/48k or PCMU/8k by payload type)", kPort);

    auto* recv_buf = static_cast<std::uint8_t*>(heap_caps_malloc(kRecvBufBytes, MALLOC_CAP_8BIT));
    bool session = false;
    std::uint32_t last_packet_ms = 0;
    std::uint16_t last_seq = 0;
    bool have_seq = false;

    for (;;) {
        const int n = ::recvfrom(sock, recv_buf, kRecvBufBytes, 0, nullptr, nullptr);
        const std::uint32_t t = now_ms();

        if (n > 0) {
            const RtpView rtp = parse_rtp(recv_buf, static_cast<std::size_t>(n));
            if (rtp.valid) {
                std::uint32_t lost = 0;
                bool stale = false;
                if (have_seq) {
                    const std::int16_t d = static_cast<std::int16_t>(rtp.seq - last_seq);
                    if (d <= 0) stale = true;            // late / duplicate (no reorder buffer yet)
                    else lost = static_cast<std::uint32_t>(d - 1);
                }
                if (!stale) {
                    last_seq = rtp.seq;
                    have_seq = true;
                    if (!session) {
                        session = true;
                        current_pt = -1; // force codec (re)selection for this stream
                        player->begin_session();
                    }
                    // (Re)build the depacketizer when the payload type changes.
                    if (static_cast<int>(rtp.pt) != current_pt) {
                        current_pt = rtp.pt;
                        const CodecSel sel = codec_for_pt(rtp.pt);
                        depack = make_depacketizer(sel.codec, StreamFormat{sel.rate, 1});
                        ESP_LOGI(kTag, "codec: %s (PT %u)", sel.name,
                                 static_cast<unsigned>(rtp.pt));
                    }
                    last_packet_ms = t;
                    if (depack) {
                        depack->on_packet(rtp.payload, rtp.payload_len, rtp.ts, rtp.marker, lost, *player);
                    }
                }
            }
        }

        if (session) {
            player->service();
            if (t - last_packet_ms > kStopSilenceMs) {
                player->end_session();
                session = false;
                have_seq = false;
                current_pt = -1;
            }
        }
    }
}

} // namespace

std::unique_ptr<IDepacketizer> make_depacketizer(Codec codec, const StreamFormat& fmt)
{
    switch (codec) {
    case Codec::L16:
        return std::make_unique<L16Depacketizer>();
    case Codec::MuLaw:
        return std::make_unique<MuLawDepacketizer>(fmt.sample_rate);
    case Codec::Aac:
        return nullptr; // wifi_audio_aac.cpp (future)
    }
    return nullptr;
}

void start(SharedState& state, bool conversation_enabled, bool rtp_enabled)
{
    g_state = &state;
    g_conversation_enabled = conversation_enabled;

    if (!rtp_enabled) {
        ESP_LOGI(kTag, "Wi-Fi RTP audio disabled by config");
        return;
    }
    if (conversation_enabled) {
        ESP_LOGI(kTag, "Wi-Fi audio disabled — conversation mode is on (mutually exclusive)");
        return;
    }

    // Internal-RAM stack: lwip + M5.Speaker, no flash ops, so PSRAM would be
    // fine too, but internal keeps it simple. Core 1 away from NimBLE.
    if (xTaskCreatePinnedToCore(receiver_task, "wifi-audio", 6144, nullptr,
                                tskIDLE_PRIORITY + 6, nullptr, 1) != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(wifi-audio) failed");
    }
}

} // namespace stackchan::app::wifi_audio
