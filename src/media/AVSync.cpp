#include "media/AVSync.h"

namespace vc {

SystemTime AVSync::ntpToSystem(NtpTimestamp ntp) {
    // NTP epoch is 1900-01-01; Unix/system_clock epoch is 1970-01-01.
    constexpr uint64_t kNtpUnixOffset = 2'208'988'800ULL;
    const int64_t unixSeconds = static_cast<int64_t>(ntp.seconds) -
                                static_cast<int64_t>(kNtpUnixOffset);
    // fraction is a Q32 fraction of a second.
    const int64_t fracNanos =
        static_cast<int64_t>((static_cast<__int128>(ntp.fraction) * 1'000'000'000) >> 32);
    const auto dur = std::chrono::seconds{unixSeconds} + std::chrono::nanoseconds{fracNanos};
    return SystemTime{std::chrono::duration_cast<SystemClock::duration>(dur)};
}

void AVSync::onSenderReport(uint32_t ssrc, uint32_t rtpTimestamp, NtpTimestamp ntp,
                            uint32_t clockRateHz) {
    if (clockRateHz == 0) return;
    anchors_[ssrc] = Anchor{rtpTimestamp, ntpToSystem(ntp), clockRateHz};
}

std::optional<SystemTime> AVSync::playoutTime(uint32_t ssrc, uint32_t rtpTimestamp) const {
    const auto it = anchors_.find(ssrc);
    if (it == anchors_.end()) return std::nullopt;
    const Anchor& a = it->second;

    // Signed 32-bit difference so the mapping works across RTP timestamp wrap.
    const int32_t deltaTicks = static_cast<int32_t>(rtpTimestamp - a.rtpTimestamp);
    const int64_t deltaNanos =
        static_cast<int64_t>((static_cast<__int128>(deltaTicks) * 1'000'000'000) / a.clockRateHz);
    return a.wall + std::chrono::duration_cast<SystemClock::duration>(
                        std::chrono::nanoseconds{deltaNanos});
}

} // namespace vc
