#pragma once

// JitterBuffer — reorder, de-duplicate, and reassemble RTP packets into
// playout-ordered EncodedFrames, absorbing network jitter via a target delay.
//
// Responsibilities:
//   * Accept RtpPackets from the receive thread (push()).
//   * Reorder by 16-bit sequence number, correctly handling wraparound.
//   * Drop duplicate packets (same sequence number seen twice).
//   * Drop "too late" packets — those older than the most recently emitted
//     frame's timestamp (their access unit already played out).
//   * Detect sequence-number gaps (missing packets) and expose them for gap
//     concealment / NACK generation.
//   * Group packets into access units (by RTP timestamp + the marker bit) and
//     emit each as an EncodedFrame once the configured target delay has elapsed
//     and all of its packets are present (or the frame is declared complete by
//     a marker bit / a newer timestamp arriving).
//
// Threading: push() runs on the receive thread, pop()/stats run on the playout
// thread. All shared state is guarded by a single std::mutex.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "common/Clock.h"
#include "common/EncodedFrame.h"
#include "common/Types.h"
#include "network/RTPHandler.h"
#include "network/RtpPacket.h"

namespace vc {

// A detected gap in the received sequence space (missing packets), suitable for
// feeding a NACK request.
struct SequenceGap {
    uint16_t firstMissing = 0; // first absent sequence number
    uint16_t lastMissing = 0;  // last absent sequence number (inclusive)
};

// Configuration for a JitterBuffer instance.
struct JitterBufferConfig {
    // How long a frame is held before becoming eligible for playout, absorbing
    // reordering/jitter. Default 50 ms.
    std::chrono::milliseconds targetDelay{50};

    // Stream descriptors so emitted EncodedFrames are tagged correctly and the
    // depacketizer picks the right reassembly path.
    MediaKind kind = MediaKind::Video;
    VideoCodec videoCodec = VideoCodec::H264;
    AudioCodec audioCodec = AudioCodec::Opus;
};

// Runtime counters, useful for tests and telemetry.
struct JitterStats {
    uint64_t pushed = 0;     // packets accepted into the buffer
    uint64_t duplicates = 0; // packets dropped as duplicates
    uint64_t late = 0;       // packets dropped for arriving too late
    uint64_t emitted = 0;    // EncodedFrames handed out via pop()
    uint64_t reordered = 0;  // packets that arrived out of sequence order
};

class JitterBuffer {
public:
    explicit JitterBuffer(JitterBufferConfig config = {});

    // Insert a received packet. `arrival` is the steady-clock instant the
    // packet was received (defaults to now()); it anchors the target-delay
    // timer for the packet's access unit. Returns false if the packet was
    // dropped (duplicate or too late), true if it was buffered.
    bool push(const RtpPacket& packet, SteadyTime arrival = MediaClock::now());

    // Emit the next access unit if one is complete AND its target delay has
    // elapsed relative to `now`. Returns std::nullopt when nothing is ready.
    // Frames are returned strictly in timestamp order.
    [[nodiscard]] std::optional<EncodedFrame> pop(SteadyTime now = MediaClock::now());

    // Report sequence-number gaps among packets currently buffered (and not yet
    // emitted). Each gap is a contiguous run of missing sequence numbers.
    [[nodiscard]] std::vector<SequenceGap> gaps() const;

    [[nodiscard]] JitterStats stats() const;

    // Number of access units currently buffered (complete or not).
    [[nodiscard]] std::size_t frameCount() const;

private:
    // One access unit being assembled, keyed by RTP timestamp.
    struct PendingFrame {
        uint32_t timestamp = 0;
        SteadyTime firstArrival{};        // when its first packet arrived
        bool hasMarker = false;           // marker bit (end of AU) seen
        uint16_t minSeq = 0;              // lowest sequence number seen
        uint16_t maxSeq = 0;              // highest sequence number seen
        bool seqInit = false;
        std::map<uint16_t, RtpPacket> packets; // keyed by sequence number
    };

    // True once we have a contiguous run of packets from minSeq..maxSeq and a
    // marker bit terminating the access unit (so no later packets are expected).
    [[nodiscard]] bool isComplete(const PendingFrame& f) const;

    // Order timestamps for playout, accounting for 32-bit RTP wraparound.
    [[nodiscard]] static bool tsLessThan(uint32_t a, uint32_t b) noexcept {
        return static_cast<uint32_t>(b - a) < 0x80000000u && a != b;
    }

    JitterBufferConfig config_;
    mutable std::mutex mu_;

    // Pending access units keyed by RTP timestamp. We keep insertion ordered by
    // timestamp via the comparator below.
    struct TsCompare {
        bool operator()(uint32_t a, uint32_t b) const noexcept {
            return tsLessThan(a, b);
        }
    };
    std::map<uint32_t, PendingFrame, TsCompare> frames_;

    bool hasEmitted_ = false;
    uint32_t lastEmittedTs_ = 0; // timestamp of the most recently emitted frame
    JitterStats stats_{};
};

} // namespace vc
