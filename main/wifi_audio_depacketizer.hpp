// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace stackchan::app::wifi_audio {

// Device playback format. Fixed at 48 kHz mono S16 for now; the field exists
// so the AAC depacketizer (added later) can carry the negotiated rate/channels
// without changing this interface.
struct StreamFormat {
    std::uint32_t sample_rate = 48000;
    std::uint8_t channels = 1;
};

// Codec-agnostic PCM output. Depacketizers convert their RTP payloads into
// mono S16 samples and write them here; the implementation (the live player)
// owns the jitter ring + speaker. push_silence() is how packet loss is
// concealed without breaking the sample clock.
class PcmSink {
public:
    virtual ~PcmSink() = default;
    virtual void push(const std::int16_t* samples, std::size_t n) = 0;
    virtual void push_silence(std::size_t n) = 0;
};

// Identifies the RTP payload format. L16 (raw PCM) ships first; Aac
// (RFC 3640 mpeg4-generic) plugs into the same interface later.
enum class Codec : std::uint8_t {
    L16,
    Aac,
};

// Converts in-order RTP payloads into PCM. The RtpReceiver hands packets in
// sequence order (it reorders / drops late packets) and reports any loss, so
// implementations only deal with a forward-moving stream.
class IDepacketizer {
public:
    virtual ~IDepacketizer() = default;

    // payload/len: RTP payload (the bytes after the 12+ byte RTP header).
    // rtp_ts: RTP timestamp (sample clock — increments by sample count).
    // marker: RTP marker bit.
    // lost: number of RTP packets lost immediately before this one (0 = none),
    //       informational; precise gap length is derived from rtp_ts.
    virtual void on_packet(const std::uint8_t* payload, std::size_t len,
                           std::uint32_t rtp_ts, bool marker,
                           std::uint32_t lost, PcmSink& out) = 0;

    // Drop all per-stream state (new talkspurt / stream restart).
    virtual void reset() = 0;
};

// Build the depacketizer for the given codec. Only L16 is wired today; Aac
// returns nullptr until wifi_audio_aac.cpp lands.
std::unique_ptr<IDepacketizer> make_depacketizer(Codec codec, const StreamFormat& fmt);

} // namespace stackchan::app::wifi_audio
