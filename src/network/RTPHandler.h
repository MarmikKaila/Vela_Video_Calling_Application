#pragma once

// RTPHandler — packetize EncodedFrames into RtpPackets and reassemble them.
//
// Packetization (send side):
//   * H.264 (RFC 6184): the EncodedFrame payload is an Annex-B access unit
//     (NAL units separated by 0x000001 / 0x00000001 start codes). Each NAL is
//     emitted as a single-NAL-unit packet if it fits the MTU payload budget, or
//     fragmented into FU-A packets (RFC 6184 §5.8) if it does not. The marker
//     bit is set on the last packet of the access unit. All packets of the
//     access unit share the frame's rtpTimestamp.
//   * Opus: one RTP packet per encoded frame (RFC 7587), marker bit set.
//
// Depacketization (receive side):
//   Accumulate the RtpPackets of one access unit (same timestamp) and rebuild
//   the EncodedFrame: reassemble FU-A fragments and re-prefix each NAL with a
//   4-byte Annex-B start code. The JitterBuffer is responsible for ordering and
//   completeness; depacketize() assumes it is handed the in-order packets of a
//   single, complete access unit.
//
// State: an RTPHandler instance owns one outbound stream's SSRC and a rolling
// 16-bit sequence number; it is NOT thread-safe (one packetizer per stream).

#include <cstdint>
#include <vector>

#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/Result.h"
#include "common/Types.h"
#include "network/RtpPacket.h"

namespace vc {

// Default maximum RTP payload (bytes) before FU-A fragmentation kicks in. Sized
// to keep the full IP/UDP/RTP datagram under a typical 1500-byte path MTU.
inline constexpr std::size_t kDefaultMaxRtpPayload = 1200;

class RTPHandler {
public:
    // Construct a packetizer for one stream. `payloadType` is the negotiated
    // dynamic PT; `ssrc` identifies the stream; `clockRateHz` is the codec
    // clock (kVideoRtpClockHz / kAudioRtpClockHz). `maxPayload` caps the RTP
    // payload size that triggers fragmentation.
    RTPHandler(uint8_t payloadType, uint32_t ssrc,
               uint32_t clockRateHz = kVideoRtpClockHz,
               std::size_t maxPayload = kDefaultMaxRtpPayload);

    // Packetize one access unit / encoded frame into ordered RtpPackets. The
    // sequence number advances across the returned packets and persists across
    // calls. Returns Error::InvalidArgument for an empty frame and
    // Error::Unsupported for codecs other than H.264 / Opus.
    [[nodiscard]] Result<std::vector<RtpPacket>, Error> packetize(const EncodedFrame& frame);

    // Reassemble the in-order, complete set of packets belonging to one access
    // unit back into an EncodedFrame. `kind`/codec describe the stream so the
    // result is tagged correctly. Returns Error::InvalidArgument on empty input
    // and Error::Protocol on malformed FU-A sequences.
    [[nodiscard]] static Result<EncodedFrame, Error> depacketize(
        const std::vector<RtpPacket>& packets, MediaKind kind,
        VideoCodec videoCodec = VideoCodec::H264,
        AudioCodec audioCodec = AudioCodec::Opus);

    [[nodiscard]] uint16_t nextSequenceNumber() const noexcept { return seq_; }
    [[nodiscard]] uint32_t ssrc() const noexcept { return ssrc_; }

private:
    [[nodiscard]] Result<std::vector<RtpPacket>, Error> packetizeH264(const EncodedFrame& frame);
    [[nodiscard]] Result<std::vector<RtpPacket>, Error> packetizeOpus(const EncodedFrame& frame);

    // Split an Annex-B buffer into NAL unit byte-ranges (payload without the
    // start codes). Returns empty if no start codes are found.
    [[nodiscard]] static std::vector<std::pair<std::size_t, std::size_t>>
    findNalUnits(const uint8_t* data, std::size_t len);

    uint8_t payloadType_;
    uint32_t ssrc_;
    uint32_t clockRateHz_;
    std::size_t maxPayload_;
    uint16_t seq_ = 0;
};

} // namespace vc
