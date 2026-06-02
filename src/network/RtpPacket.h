#pragma once

// RtpPacket — an RFC 3550 RTP packet model (header + payload).
//
// This is the wire representation of a single RTP packet. It is the seam
// between the RTPHandler (which packetizes EncodedFrames into RtpPackets and
// reassembles them back) and the network transport, and it is reused verbatim
// by the SFU module in Wave B for forwarding. Keep it dependency-light and
// header-only so any module can include it without linking.
//
// On-the-wire layout (RFC 3550 §5.1), all multi-byte fields big-endian:
//
//    0                   1                   2                   3
//    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                           timestamp                           |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |           synchronization source (SSRC) identifier            |
//   +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
//   |            contributing source (CSRC) identifiers             |
//   |                             ....                              |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//   |                          payload ...                          |
//   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// The fixed header is 12 bytes; each CSRC adds 4 bytes. This model does not
// parse header extensions beyond recording the X bit and skipping the extension
// body, which is sufficient for the codecs we carry (H.264 / Opus).

#include <cstdint>
#include <vector>

#include "common/Error.h"
#include "common/Result.h"

namespace vc {

// A parsed/buildable RTP packet. Default-constructed it is a valid version-2
// packet with empty payload; set fields then serialize(), or parse() raw bytes.
struct RtpPacket {
    static constexpr std::size_t kFixedHeaderBytes = 12;
    static constexpr uint8_t kVersion = 2;

    // --- Header fields (see layout above) ------------------------------------
    uint8_t version = kVersion; // RTP version, always 2.
    bool padding = false;       // P: trailing padding bytes present.
    bool extension = false;     // X: a header extension follows the CSRC list.
    bool marker = false;        // M: codec-defined; for video, last packet of a
                                //    frame/access unit.
    uint8_t payloadType = 0;    // PT: 7-bit dynamic payload type.
    uint16_t sequenceNumber = 0; // Per-packet counter; wraps modulo 2^16.
    uint32_t timestamp = 0;      // Codec clock units (kVideoRtpClockHz etc.).
    uint32_t ssrc = 0;           // Synchronization source identifier.

    // Contributing sources (only present on mixed streams; usually empty).
    std::vector<uint32_t> csrcs;

    // Codec payload (e.g. a NAL unit, FU-A fragment, or Opus frame).
    std::vector<uint8_t> payload;

    // Serialize this packet into a contiguous big-endian byte buffer ready for
    // the wire. CSRC count is clamped to its 4-bit field (max 15).
    [[nodiscard]] std::vector<uint8_t> serialize() const {
        const std::size_t cc = csrcs.size() > 15 ? 15 : csrcs.size();
        std::vector<uint8_t> out;
        out.reserve(kFixedHeaderBytes + cc * 4 + payload.size());

        uint8_t b0 = static_cast<uint8_t>((version & 0x03) << 6);
        if (padding)   b0 |= 0x20;
        if (extension) b0 |= 0x10;
        b0 |= static_cast<uint8_t>(cc & 0x0F);
        out.push_back(b0);

        uint8_t b1 = static_cast<uint8_t>(payloadType & 0x7F);
        if (marker) b1 |= 0x80;
        out.push_back(b1);

        putU16(out, sequenceNumber);
        putU32(out, timestamp);
        putU32(out, ssrc);
        for (std::size_t i = 0; i < cc; ++i) putU32(out, csrcs[i]);

        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    // Parse a raw datagram into an RtpPacket. Returns Error::Protocol on any
    // malformed/truncated input or a non-2 version. Header extensions are
    // recognized (X bit retained) and their body is skipped over.
    [[nodiscard]] static Result<RtpPacket, Error> parse(const uint8_t* data,
                                                        std::size_t len) {
        if (data == nullptr || len < kFixedHeaderBytes) return fail(Error::Protocol);

        RtpPacket p;
        const uint8_t b0 = data[0];
        p.version = static_cast<uint8_t>((b0 >> 6) & 0x03);
        if (p.version != kVersion) return fail(Error::Protocol);
        p.padding = (b0 & 0x20) != 0;
        p.extension = (b0 & 0x10) != 0;
        const std::size_t cc = b0 & 0x0F;

        const uint8_t b1 = data[1];
        p.marker = (b1 & 0x80) != 0;
        p.payloadType = static_cast<uint8_t>(b1 & 0x7F);

        p.sequenceNumber = getU16(data + 2);
        p.timestamp = getU32(data + 4);
        p.ssrc = getU32(data + 8);

        std::size_t offset = kFixedHeaderBytes;
        if (len < offset + cc * 4) return fail(Error::Protocol);
        p.csrcs.reserve(cc);
        for (std::size_t i = 0; i < cc; ++i) {
            p.csrcs.push_back(getU32(data + offset));
            offset += 4;
        }

        // Optional header extension: 16-bit profile id + 16-bit length (in
        // 32-bit words), followed by that many words. We skip the body.
        if (p.extension) {
            if (len < offset + 4) return fail(Error::Protocol);
            const uint16_t words = getU16(data + offset + 2);
            offset += 4 + static_cast<std::size_t>(words) * 4;
            if (len < offset) return fail(Error::Protocol);
        }

        // Trailing padding: the last byte counts the padding octets (incl. itself).
        std::size_t end = len;
        if (p.padding) {
            if (end <= offset) return fail(Error::Protocol);
            const std::size_t pad = data[len - 1];
            if (pad == 0 || pad > end - offset) return fail(Error::Protocol);
            end -= pad;
        }

        p.payload.assign(data + offset, data + end);
        return p;
    }

    [[nodiscard]] static Result<RtpPacket, Error> parse(const std::vector<uint8_t>& bytes) {
        return parse(bytes.data(), bytes.size());
    }

private:
    static void putU16(std::vector<uint8_t>& out, uint16_t v) {
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    static void putU32(std::vector<uint8_t>& out, uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    [[nodiscard]] static uint16_t getU16(const uint8_t* p) {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }
    [[nodiscard]] static uint32_t getU32(const uint8_t* p) {
        return (static_cast<uint32_t>(p[0]) << 24) |
               (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) |
               static_cast<uint32_t>(p[3]);
    }
};

// Compare RTP 16-bit sequence numbers with wraparound (RFC 1982 serial
// arithmetic). Returns true iff `a` is logically *earlier* than `b`, treating
// distances over half the sequence space as a wrap.
[[nodiscard]] inline bool seqLessThan(uint16_t a, uint16_t b) noexcept {
    return static_cast<uint16_t>(b - a) < 0x8000 && a != b;
}

// Forward distance from `a` to `b` accounting for wraparound (number of
// sequence steps to advance from a to b).
[[nodiscard]] inline uint16_t seqDistance(uint16_t a, uint16_t b) noexcept {
    return static_cast<uint16_t>(b - a);
}

} // namespace vc
