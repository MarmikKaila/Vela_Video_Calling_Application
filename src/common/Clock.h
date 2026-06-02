#pragma once

// Time primitives shared across the media pipeline.
//
// Two clock domains matter and must never be confused:
//   * SteadyTime  — monotonic, never jumps; used for *scheduling*, latency
//                   measurement, jitter-buffer playout timing.
//   * SystemTime  — wall clock; only used to build NTP timestamps for RTCP
//                   Sender Reports, which AVSync uses to align audio & video
//                   streams that have independent RTP timestamp bases.
//
// RTP timestamps are derived from SteadyTime against a per-stream epoch and a
// codec clock rate (see Types.h: kVideoRtpClockHz / kAudioRtpClockHz).

#include <chrono>
#include <cstdint>

namespace vc {

using SteadyClock = std::chrono::steady_clock;
using SteadyTime = SteadyClock::time_point;
using SystemClock = std::chrono::system_clock;
using SystemTime = SystemClock::time_point;

// 64-bit NTP timestamp (RFC 3550 §4): high 32 bits = seconds since 1900,
// low 32 bits = fractional second. Carried in RTCP Sender Reports.
struct NtpTimestamp {
    uint32_t seconds = 0;
    uint32_t fraction = 0;
};

class MediaClock {
public:
    [[nodiscard]] static SteadyTime now() noexcept { return SteadyClock::now(); }
    [[nodiscard]] static SystemTime wallNow() noexcept { return SystemClock::now(); }

    // Convert a monotonic capture instant into a 32-bit RTP timestamp ticking at
    // `clockRateHz`, relative to the stream's epoch (the SteadyTime of the first
    // sample). Wraps modulo 2^32, exactly as RTP requires.
    [[nodiscard]] static uint32_t toRtp(SteadyTime t, SteadyTime epoch,
                                        uint32_t clockRateHz) noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t - epoch).count();
        const auto ticks = (static_cast<__int128>(elapsed) * clockRateHz) / 1'000'000'000;
        return static_cast<uint32_t>(static_cast<uint64_t>(ticks));
    }

    // Map a wall-clock instant to an NTP timestamp for RTCP SR generation.
    [[nodiscard]] static NtpTimestamp toNtp(SystemTime wall) noexcept {
        // Seconds between 1900-01-01 (NTP epoch) and 1970-01-01 (Unix epoch).
        constexpr uint64_t kNtpUnixOffset = 2'208'988'800ULL;
        const auto sinceEpoch = wall.time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch);
        const auto frac = std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch - secs).count();
        NtpTimestamp out;
        out.seconds = static_cast<uint32_t>(static_cast<uint64_t>(secs.count()) + kNtpUnixOffset);
        out.fraction = static_cast<uint32_t>((static_cast<__int128>(frac) << 32) / 1'000'000'000);
        return out;
    }
};

} // namespace vc
