#include "signaling/RtpTransport.h"

#include <utility>
#include <vector>

#include "signaling/DtlsSrtp.h"

namespace vc::signaling {

RtpTransport::RtpTransport() = default;
RtpTransport::~RtpTransport() = default;

Status RtpTransport::initialize(const NATConfig& cfg, Callbacks cbs) {
    cbs_ = std::move(cbs);

    NATCallbacks nat;
    nat.onLocalCandidate = [this](const std::string& cand) {
        if (cbs_.onLocalCandidate) cbs_.onLocalCandidate(cand);
    };
    nat.onGatheringDone = [this]() {
        if (cbs_.onGatheringDone) cbs_.onGatheringDone();
    };
    nat.onStateChanged = [this](IceState s) {
        if (s == IceState::Connected || s == IceState::Completed) {
            iceConnected_.store(true);
            maybeStartHandshake();  // begins DTLS once the peer fp is also known
        }
        if (cbs_.onStateChanged) cbs_.onStateChanged(s);
    };
    nat.onData = [this](const std::uint8_t* data, std::size_t len) {
        if (len == 0) return;
        if (secured_) {
            const std::uint8_t b = data[0];
            if (b >= 20 && b <= 63) {           // DTLS record
                if (dtls_) dtls_->feedDtls(data, len);
                return;
            }
            if (b >= 128 && b <= 191) {          // SRTP (RTP/RTCP)
                if (!dtls_ || !dtls_->ready() || !cbs_.onPacket) return;
                std::vector<std::uint8_t> buf(data, data + len);
                if (!dtls_->unprotect(buf)) return;
                auto pkt = RtpPacket::parse(buf);
                if (pkt) cbs_.onPacket(pkt.value());
                return;
            }
            return;  // unknown leading byte: drop
        }
        // Insecure path: bytes are a raw RTP datagram.
        if (!cbs_.onPacket) return;
        auto pkt = RtpPacket::parse(data, len);
        if (pkt) cbs_.onPacket(pkt.value());
    };

    return nat_.initialize(cfg, std::move(nat));
}

void RtpTransport::enableSecurity(bool asClient) {
    secured_ = true;
    asClient_ = asClient;
    dtls_ = std::make_unique<DtlsSrtp>();
}

std::string RtpTransport::localFingerprint() const { return DtlsSrtp::localFingerprint(); }

bool RtpTransport::secureReady() const { return dtls_ && dtls_->ready(); }

void RtpTransport::setRemoteFingerprint(std::string fingerprint) {
    {
        std::lock_guard<std::mutex> lk(startMu_);
        remoteFp_ = std::move(fingerprint);
    }
    maybeStartHandshake();
}

void RtpTransport::maybeStartHandshake() {
    std::lock_guard<std::mutex> lk(startMu_);
    if (!secured_ || handshakeStarted_ || !dtls_) return;
    if (!iceConnected_.load() || remoteFp_.empty()) return;  // need both
    handshakeStarted_ = true;
    const auto role = asClient_ ? DtlsSrtp::Role::Client : DtlsSrtp::Role::Server;
    (void)dtls_->start(
        role, remoteFp_,
        [this](const std::uint8_t* d, std::size_t n) { (void)nat_.send(d, n); },
        [](bool /*ok*/) {});
}

Status RtpTransport::send(const RtpPacket& pkt) {
    std::vector<std::uint8_t> bytes = pkt.serialize();
    if (secured_) {
        // Drop until the DTLS handshake has produced SRTP keys (a brief window
        // right after ICE connects). Media resumes automatically once ready.
        if (!dtls_ || !dtls_->ready()) return fail(Error::NotInitialized);
        if (!dtls_->protect(bytes)) return fail(Error::Internal);
    }
    return nat_.send(bytes.data(), bytes.size());
}

}  // namespace vc::signaling
