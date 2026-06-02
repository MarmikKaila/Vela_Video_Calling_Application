#include "network/JitterBuffer.h"

#include <utility>

namespace vc {

JitterBuffer::JitterBuffer(JitterBufferConfig config) : config_(config) {}

bool JitterBuffer::push(const RtpPacket& packet, SteadyTime arrival) {
    std::lock_guard<std::mutex> lock(mu_);

    // Too late: this packet belongs to an access unit at/older than the last
    // one we already emitted. Its frame has played out — discard it.
    if (hasEmitted_ &&
        (packet.timestamp == lastEmittedTs_ || tsLessThan(packet.timestamp, lastEmittedTs_))) {
        ++stats_.late;
        return false;
    }

    auto& frame = frames_[packet.timestamp];
    if (frame.packets.empty()) {
        frame.timestamp = packet.timestamp;
        frame.firstArrival = arrival;
    }

    // Duplicate: same sequence number already buffered for this frame.
    if (frame.packets.find(packet.sequenceNumber) != frame.packets.end()) {
        ++stats_.duplicates;
        return false;
    }

    // Track whether this arrived out of order relative to packets already held.
    if (frame.seqInit && seqLessThan(packet.sequenceNumber, frame.maxSeq)) {
        ++stats_.reordered;
    }

    if (!frame.seqInit) {
        frame.minSeq = packet.sequenceNumber;
        frame.maxSeq = packet.sequenceNumber;
        frame.seqInit = true;
    } else {
        if (seqLessThan(packet.sequenceNumber, frame.minSeq)) frame.minSeq = packet.sequenceNumber;
        if (seqLessThan(frame.maxSeq, packet.sequenceNumber)) frame.maxSeq = packet.sequenceNumber;
    }

    if (packet.marker) frame.hasMarker = true;
    frame.packets.emplace(packet.sequenceNumber, packet);
    ++stats_.pushed;
    return true;
}

bool JitterBuffer::isComplete(const PendingFrame& f) const {
    // Audio: a single packet (with marker) is a complete frame.
    if (config_.kind == MediaKind::Audio) {
        return !f.packets.empty();
    }

    // Video: need the marker bit (end of access unit) AND a contiguous run of
    // sequence numbers from minSeq..maxSeq with no gaps.
    if (!f.hasMarker || f.packets.empty()) return false;

    const uint16_t span = seqDistance(f.minSeq, f.maxSeq); // maxSeq - minSeq mod 2^16
    if (static_cast<std::size_t>(span) + 1 != f.packets.size()) return false;
    return true;
}

std::optional<EncodedFrame> JitterBuffer::pop(SteadyTime now) {
    std::lock_guard<std::mutex> lock(mu_);

    while (!frames_.empty()) {
        // frames_ is ordered by timestamp (wraparound-aware): front is oldest.
        auto it = frames_.begin();
        PendingFrame& f = it->second;

        const bool delayElapsed = (now - f.firstArrival) >= config_.targetDelay;
        if (!delayElapsed) return std::nullopt; // hold for jitter absorption

        if (!isComplete(f)) {
            // Delay has elapsed but the frame is still incomplete. For video we
            // cannot decode a partial access unit, so drop it and advance,
            // leaving the gap visible via gaps() for upstream concealment/NACK.
            // (Holding longer only adds latency once the target delay passed.)
            const uint32_t ts = f.timestamp;
            frames_.erase(it);
            hasEmitted_ = true;
            lastEmittedTs_ = ts;
            continue;
        }

        // Assemble the in-order packet list for depacketization. We must walk
        // minSeq..maxSeq with 16-bit wraparound rather than iterating the map by
        // raw key: across a 65535->0 wrap the raw-key order would place the
        // higher sequence number last, reversing FU-A start/end fragments.
        std::vector<RtpPacket> ordered;
        ordered.reserve(f.packets.size());
        for (uint16_t seq = f.minSeq;; seq = static_cast<uint16_t>(seq + 1)) {
            auto pit = f.packets.find(seq);
            if (pit != f.packets.end()) ordered.push_back(pit->second);
            if (seq == f.maxSeq) break;
        }

        const uint32_t ts = f.timestamp;
        frames_.erase(it);

        auto result = RTPHandler::depacketize(ordered, config_.kind,
                                              config_.videoCodec, config_.audioCodec);
        hasEmitted_ = true;
        lastEmittedTs_ = ts;

        if (!result) {
            // Malformed access unit: skip it but keep advancing.
            continue;
        }
        ++stats_.emitted;
        return std::move(result).value();
    }
    return std::nullopt;
}

std::vector<SequenceGap> JitterBuffer::gaps() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SequenceGap> result;

    for (const auto& kv : frames_) {
        const PendingFrame& f = kv.second;
        if (!f.seqInit || f.packets.empty()) continue;

        // Walk the contiguous expected range minSeq..maxSeq and report runs of
        // sequence numbers absent from the buffer.
        uint16_t expected = f.minSeq;
        const uint16_t end = f.maxSeq;
        bool inGap = false;
        SequenceGap cur{};

        // Iterate inclusive of `end`, advancing with wraparound.
        while (true) {
            const bool present = f.packets.find(expected) != f.packets.end();
            if (!present) {
                if (!inGap) {
                    inGap = true;
                    cur.firstMissing = expected;
                }
                cur.lastMissing = expected;
            } else if (inGap) {
                result.push_back(cur);
                inGap = false;
            }
            if (expected == end) break;
            expected = static_cast<uint16_t>(expected + 1);
        }
        if (inGap) result.push_back(cur);
    }
    return result;
}

JitterStats JitterBuffer::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

std::size_t JitterBuffer::frameCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return frames_.size();
}

} // namespace vc
