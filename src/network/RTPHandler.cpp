#include "network/RTPHandler.h"

#include <algorithm>
#include <utility>

namespace vc {

namespace {

// H.264 NAL unit header (1 byte): F(1) NRI(2) Type(5).
constexpr uint8_t kNalTypeMask = 0x1F;
constexpr uint8_t kNalFuA = 28; // FU-A fragmentation unit type.

// FU-A indicator/header bit masks (RFC 6184 §5.8).
constexpr uint8_t kFuStart = 0x80; // S bit in the FU header.
constexpr uint8_t kFuEnd = 0x40;   // E bit in the FU header.

} // namespace

RTPHandler::RTPHandler(uint8_t payloadType, uint32_t ssrc, uint32_t clockRateHz,
                       std::size_t maxPayload)
    : payloadType_(payloadType),
      ssrc_(ssrc),
      clockRateHz_(clockRateHz),
      maxPayload_(maxPayload == 0 ? kDefaultMaxRtpPayload : maxPayload) {}

std::vector<std::pair<std::size_t, std::size_t>>
RTPHandler::findNalUnits(const uint8_t* data, std::size_t len) {
    // Locate Annex-B start codes (0x000001 or 0x00000001) and return the
    // [begin,end) range of each NAL unit body (start codes excluded).
    std::vector<std::pair<std::size_t, std::size_t>> ranges;

    // Record, for each start code found, where the start code begins and where
    // the NAL body begins (just past the code). This lets us trim the next
    // start code off the current NAL's tail without re-guessing its length.
    struct Marker { std::size_t codeBegin; std::size_t bodyBegin; };
    std::vector<Marker> markers;

    std::size_t i = 0;
    while (i + 3 <= len) {
        if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
            // A 4-byte code is a 3-byte code with one extra leading zero.
            std::size_t codeBegin = i;
            if (i >= 1 && data[i - 1] == 0x00 && !markers.empty() &&
                markers.back().bodyBegin <= i - 1) {
                codeBegin = i - 1;
            }
            markers.push_back({codeBegin, i + 3});
            i += 3;
        } else {
            ++i;
        }
    }

    for (std::size_t s = 0; s < markers.size(); ++s) {
        const std::size_t begin = markers[s].bodyBegin;
        const std::size_t end =
            (s + 1 < markers.size()) ? markers[s + 1].codeBegin : len;
        if (end > begin) ranges.emplace_back(begin, end);
    }
    return ranges;
}

Result<std::vector<RtpPacket>, Error> RTPHandler::packetize(const EncodedFrame& frame) {
    if (frame.empty()) return fail(Error::InvalidArgument);

    if (frame.kind == MediaKind::Video) {
        if (frame.videoCodec != VideoCodec::H264) return fail(Error::Unsupported);
        return packetizeH264(frame);
    }
    if (frame.audioCodec != AudioCodec::Opus) return fail(Error::Unsupported);
    return packetizeOpus(frame);
}

Result<std::vector<RtpPacket>, Error> RTPHandler::packetizeOpus(const EncodedFrame& frame) {
    RtpPacket p;
    p.payloadType = payloadType_;
    p.ssrc = ssrc_;
    p.timestamp = frame.rtpTimestamp;
    p.sequenceNumber = seq_++;
    p.marker = true; // one packet == one complete frame.
    p.payload.assign(frame.data(), frame.data() + frame.size());
    return std::vector<RtpPacket>{std::move(p)};
}

Result<std::vector<RtpPacket>, Error> RTPHandler::packetizeH264(const EncodedFrame& frame) {
    const auto nals = findNalUnits(frame.data(), frame.size());
    if (nals.empty()) return fail(Error::Protocol);

    std::vector<RtpPacket> packets;

    for (std::size_t n = 0; n < nals.size(); ++n) {
        const std::size_t begin = nals[n].first;
        const std::size_t end = nals[n].second;
        const std::size_t nalLen = end - begin;
        const uint8_t* nal = frame.data() + begin;

        if (nalLen <= maxPayload_) {
            // Single NAL unit packet: payload is the NAL verbatim.
            RtpPacket p;
            p.payloadType = payloadType_;
            p.ssrc = ssrc_;
            p.timestamp = frame.rtpTimestamp;
            p.sequenceNumber = seq_++;
            p.payload.assign(nal, nal + nalLen);
            packets.push_back(std::move(p));
        } else {
            // FU-A fragmentation. The original NAL header is replaced by a
            // 2-byte FU indicator + FU header; the NAL header's F/NRI are kept.
            const uint8_t nalHeader = nal[0];
            const uint8_t fuIndicator =
                static_cast<uint8_t>((nalHeader & 0xE0) | kNalFuA);
            const uint8_t origType = static_cast<uint8_t>(nalHeader & kNalTypeMask);

            std::size_t pos = 1; // skip the original 1-byte NAL header
            // Each FU-A packet carries 2 header bytes, so payload budget shrinks.
            const std::size_t chunk = (maxPayload_ > 2) ? (maxPayload_ - 2) : 1;

            bool first = true;
            while (pos < nalLen) {
                const std::size_t take = std::min(chunk, nalLen - pos);
                const bool last = (pos + take >= nalLen);

                uint8_t fuHeader = origType;
                if (first) fuHeader |= kFuStart;
                if (last) fuHeader |= kFuEnd;

                RtpPacket p;
                p.payloadType = payloadType_;
                p.ssrc = ssrc_;
                p.timestamp = frame.rtpTimestamp;
                p.sequenceNumber = seq_++;
                p.payload.reserve(2 + take);
                p.payload.push_back(fuIndicator);
                p.payload.push_back(fuHeader);
                p.payload.insert(p.payload.end(), nal + pos, nal + pos + take);
                packets.push_back(std::move(p));

                pos += take;
                first = false;
            }
        }
    }

    if (packets.empty()) return fail(Error::Protocol);
    // Marker bit marks the last packet of the access unit.
    packets.back().marker = true;
    return packets;
}

Result<EncodedFrame, Error> RTPHandler::depacketize(const std::vector<RtpPacket>& packets,
                                                    MediaKind kind,
                                                    VideoCodec videoCodec,
                                                    AudioCodec audioCodec) {
    if (packets.empty()) return fail(Error::InvalidArgument);

    EncodedFrame frame;
    frame.kind = kind;
    frame.rtpTimestamp = packets.front().timestamp;

    if (kind == MediaKind::Audio) {
        frame.audioCodec = audioCodec;
        // Opus: a single packet carries the whole frame.
        const auto& pl = packets.front().payload;
        frame.setPayload(std::vector<uint8_t>(pl.begin(), pl.end()));
        return frame;
    }

    frame.videoCodec = videoCodec;
    static constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

    std::vector<uint8_t> out;
    std::vector<uint8_t> fuAccum; // current FU-A NAL being reassembled
    bool inFu = false;
    uint8_t fuNalHeader = 0;
    bool sawKeyframe = false;

    auto recordType = [&](uint8_t nalType) {
        // IDR slice (5), or SPS(7)/PPS(8) bundled with an IDR, mark a keyframe.
        if (nalType == 5 || nalType == 7 || nalType == 8) sawKeyframe = true;
    };

    for (const auto& p : packets) {
        if (p.payload.empty()) return fail(Error::Protocol);
        const uint8_t type = static_cast<uint8_t>(p.payload[0] & kNalTypeMask);

        if (type == kNalFuA) {
            if (p.payload.size() < 2) return fail(Error::Protocol);
            const uint8_t fuIndicator = p.payload[0];
            const uint8_t fuHeader = p.payload[1];
            const bool start = (fuHeader & kFuStart) != 0;
            const bool end = (fuHeader & kFuEnd) != 0;
            const uint8_t origType = static_cast<uint8_t>(fuHeader & kNalTypeMask);

            if (start) {
                if (inFu) return fail(Error::Protocol); // unterminated prior FU
                inFu = true;
                fuAccum.clear();
                // Rebuild the original NAL header: F/NRI from indicator, type
                // from the FU header.
                fuNalHeader = static_cast<uint8_t>((fuIndicator & 0xE0) | origType);
                fuAccum.push_back(fuNalHeader);
            } else if (!inFu) {
                return fail(Error::Protocol); // middle/end without a start
            }

            fuAccum.insert(fuAccum.end(), p.payload.begin() + 2, p.payload.end());

            if (end) {
                if (!inFu) return fail(Error::Protocol);
                recordType(static_cast<uint8_t>(fuNalHeader & kNalTypeMask));
                out.insert(out.end(), kStartCode, kStartCode + 4);
                out.insert(out.end(), fuAccum.begin(), fuAccum.end());
                inFu = false;
                fuAccum.clear();
            }
        } else {
            // Single NAL unit packet.
            if (inFu) return fail(Error::Protocol); // FU left unterminated
            recordType(type);
            out.insert(out.end(), kStartCode, kStartCode + 4);
            out.insert(out.end(), p.payload.begin(), p.payload.end());
        }
    }

    if (inFu) return fail(Error::Protocol); // dangling fragment

    frame.keyframe = sawKeyframe;
    frame.setPayload(std::move(out));
    return frame;
}

} // namespace vc
