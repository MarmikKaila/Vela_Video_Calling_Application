#pragma once

// DtlsSrtp — DTLS-SRTP for the native media transport (RFC 5763/5764).
//
// Establishes a DTLS handshake over an already-connected ICE channel, then uses
// the negotiated keys (exported via the use_srtp extension) to drive libsrtp,
// encrypting/authenticating every RTP packet (SRTP). This is the same security
// model browsers use: the peers exchange self-signed-certificate *fingerprints*
// over the (trusted) signaling channel, and the DTLS handshake is accepted only
// if the peer's certificate matches that fingerprint — so a network attacker
// can neither read nor inject media (no MITM).
//
// The handshake runs on a dedicated thread that owns the OpenSSL SSL object;
// incoming DTLS records are fed in from the transport thread via feedDtls().
// After it completes, protect()/unprotect() are called inline on the media
// threads (guarded; the send side may be touched by audio + video concurrently).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common/Error.h"

// Opaque underlying types (avoid leaking OpenSSL/libsrtp headers here).
struct ssl_st;
struct bio_st;
struct srtp_ctx_t_;

namespace vc::signaling {

class DtlsSrtp {
public:
    enum class Role { Client, Server };  // DTLS active vs passive
    using OutboundFn = std::function<void(const std::uint8_t*, std::size_t)>;
    using DoneFn = std::function<void(bool ok)>;

    DtlsSrtp() = default;
    ~DtlsSrtp();
    DtlsSrtp(const DtlsSrtp&) = delete;
    DtlsSrtp& operator=(const DtlsSrtp&) = delete;

    // SHA-256 fingerprint of this process's DTLS certificate, formatted
    // "sha-256 AA:BB:..". Stable for the process; safe to send in signaling
    // before any handshake.
    [[nodiscard]] static std::string localFingerprint();

    // Bundle/extract an ICE description and a fingerprint in one signaling
    // payload (kept separate from the ICE lines so libjuice never sees the fp).
    [[nodiscard]] static std::string packDescription(const std::string& ice,
                                                     const std::string& fingerprint);
    static void unpackDescription(const std::string& payload, std::string& ice,
                                  std::string& fingerprint);

    // Begin the handshake on a background thread. `out` sends DTLS records over
    // the wire (ICE); `done(ok)` fires when the handshake + SRTP keying finish.
    [[nodiscard]] Status start(Role role, std::string remoteFingerprint,
                               OutboundFn out, DoneFn done);

    // Feed one inbound DTLS record (from the transport/ICE thread).
    void feedDtls(const std::uint8_t* data, std::size_t len);

    [[nodiscard]] bool ready() const { return ready_.load(std::memory_order_acquire); }

    // SRTP protect/unprotect in place (post-handshake). False on failure.
    [[nodiscard]] bool protect(std::vector<std::uint8_t>& pkt);
    [[nodiscard]] bool unprotect(std::vector<std::uint8_t>& pkt);

    void stop();

private:
    void run();
    void flushOutbound();
    [[nodiscard]] bool verifyPeerFingerprint();
    [[nodiscard]] bool deriveSrtpKeys();

    Role role_ = Role::Client;
    std::string remoteFp_;
    OutboundFn out_;
    DoneFn done_;

    ssl_st* ssl_ = nullptr;
    bio_st* rbio_ = nullptr;  // owned by ssl_ after SSL_set_bio
    bio_st* wbio_ = nullptr;
    srtp_ctx_t_* srtpIn_ = nullptr;
    srtp_ctx_t_* srtpOut_ = nullptr;

    std::thread thread_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::queue<std::vector<std::uint8_t>> inQ_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> stop_{false};
    std::mutex outMu_;  // guards srtpOut_ (audio + video send threads)
    std::mutex inMu_;   // guards srtpIn_
};

}  // namespace vc::signaling
