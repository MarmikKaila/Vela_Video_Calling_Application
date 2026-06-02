#pragma once

// AVSync — aligns audio and video that carry independent RTP timestamp bases.
//
// An RTP stream's timestamps tick at a codec-defined rate (90 kHz video,
// 48 kHz audio) from a random origin, so video TS and audio TS are not directly
// comparable. RTCP Sender Reports (RFC 3550 §6.4) periodically publish, per
// SSRC, a correspondence between that stream's RTP timestamp and a wall-clock
// NTP instant. Given one such (rtp, ntp) anchor and the clock rate, any RTP
// timestamp on that stream maps to wall-clock time:
//
//     wall(ts) = ntpAnchor + (int32_t)(ts - rtpAnchor) / clockRateHz
//
// Mapping both streams onto the shared wall-clock timeline lets the renderer
// release the audio sample and the video frame that share a wall instant
// together — that is lip-sync. AVSync just provides the mapping; the playout
// scheduler decides when "now" has reached a frame's wall time.
//
// Thread-safety: not synchronized. Feed reports and query from one thread (the
// receive/render thread), or guard externally.

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "common/Clock.h"

namespace vc {

class AVSync {
public:
    // Record an RTCP Sender Report anchor for `ssrc`: the stream's RTP timestamp
    // and the NTP wall instant it was captured at, plus the stream's clock rate.
    void onSenderReport(uint32_t ssrc, uint32_t rtpTimestamp, NtpTimestamp ntp,
                        uint32_t clockRateHz);

    // Map an RTP timestamp on `ssrc` to its wall-clock playout instant. Returns
    // nullopt if no Sender Report has been seen for that SSRC yet.
    [[nodiscard]] std::optional<SystemTime> playoutTime(uint32_t ssrc,
                                                        uint32_t rtpTimestamp) const;

    [[nodiscard]] bool hasMapping(uint32_t ssrc) const {
        return anchors_.find(ssrc) != anchors_.end();
    }

    // Exposed for testing / reuse: convert an NTP timestamp to system_clock time.
    [[nodiscard]] static SystemTime ntpToSystem(NtpTimestamp ntp);

private:
    struct Anchor {
        uint32_t rtpTimestamp;
        SystemTime wall;     // ntp converted to system_clock
        uint32_t clockRateHz;
    };
    std::unordered_map<uint32_t, Anchor> anchors_;
};

} // namespace vc
