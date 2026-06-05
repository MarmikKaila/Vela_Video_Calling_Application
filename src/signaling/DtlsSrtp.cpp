#include "signaling/DtlsSrtp.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/srtp.h>
#include <openssl/x509.h>
#include <srtp2/srtp.h>

namespace vc::signaling {

namespace {

// SRTP_AES128_CM_SHA1_80 key sizes (RFC 3711): 16-byte key + 14-byte salt.
constexpr int kKeyLen = 16;
constexpr int kSaltLen = 14;
constexpr int kMasterLen = kKeyLen + kSaltLen;  // 30, what libsrtp wants

// Format a certificate's SHA-256 digest as "sha-256 AA:BB:..".
std::string fingerprintOf(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!cert || X509_digest(cert, EVP_sha256(), md, &n) != 1) return {};
    static const char* hex = "0123456789ABCDEF";
    std::string s = "sha-256 ";
    for (unsigned int i = 0; i < n; ++i) {
        if (i) s += ':';
        s += hex[md[i] >> 4];
        s += hex[md[i] & 0xF];
    }
    return s;
}

std::string normalizeFp(std::string s) {
    // Trim whitespace and uppercase so comparison is robust.
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Process-wide DTLS identity + context. One self-signed cert/key, reused for
// every connection (one stable fingerprint per process).
struct Global {
    SSL_CTX* ctx = nullptr;
    X509* cert = nullptr;
    EVP_PKEY* key = nullptr;
};

int acceptAnyCert(int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) {
    return 1;  // self-signed; trust comes from the fingerprint check, not a CA
}

Global& global() {
    static Global g = [] {
        srtp_init();
        Global gg;
        gg.key = EVP_EC_gen("P-256");
        gg.cert = X509_new();
        X509_set_version(gg.cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(gg.cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(gg.cert), -3600);
        X509_gmtime_adj(X509_getm_notAfter(gg.cert), 365L * 24 * 3600);
        X509_set_pubkey(gg.cert, gg.key);
        X509_NAME* name = X509_get_subject_name(gg.cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("vela"), -1, -1, 0);
        X509_set_issuer_name(gg.cert, name);
        X509_sign(gg.cert, gg.key, EVP_sha256());

        gg.ctx = SSL_CTX_new(DTLS_method());
        SSL_CTX_set_verify(gg.ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           acceptAnyCert);
        SSL_CTX_use_certificate(gg.ctx, gg.cert);
        SSL_CTX_use_PrivateKey(gg.ctx, gg.key);
        SSL_CTX_set_tlsext_use_srtp(gg.ctx, "SRTP_AES128_CM_SHA1_80");
        return gg;
    }();
    return g;
}

}  // namespace

std::string DtlsSrtp::localFingerprint() { return fingerprintOf(global().cert); }

std::string DtlsSrtp::packDescription(const std::string& ice, const std::string& fingerprint) {
    return ice + "\n#fp " + fingerprint;
}

void DtlsSrtp::unpackDescription(const std::string& payload, std::string& ice,
                                 std::string& fingerprint) {
    const auto pos = payload.find("\n#fp ");
    if (pos == std::string::npos) {
        ice = payload;
        fingerprint.clear();
        return;
    }
    ice = payload.substr(0, pos);
    fingerprint = payload.substr(pos + 5);
}

DtlsSrtp::~DtlsSrtp() { stop(); }

Status DtlsSrtp::start(Role role, std::string remoteFingerprint, OutboundFn out, DoneFn done) {
    if (ssl_) return fail(Error::AlreadyRunning);
    role_ = role;
    remoteFp_ = std::move(remoteFingerprint);
    out_ = std::move(out);
    done_ = std::move(done);

    ssl_ = SSL_new(global().ctx);
    if (!ssl_) return fail(Error::Internal);
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(rbio_, -1);  // empty read => retry (WANT_READ), not EOF
    BIO_set_mem_eof_return(wbio_, -1);
    SSL_set_bio(ssl_, rbio_, wbio_);  // ssl_ now owns the BIOs
    DTLS_set_link_mtu(ssl_, 1200);    // fragment handshake to fit typical UDP
    if (role_ == Role::Client) SSL_set_connect_state(ssl_);
    else SSL_set_accept_state(ssl_);

    thread_ = std::thread([this] { run(); });
    return ok();
}

void DtlsSrtp::feedDtls(const std::uint8_t* data, std::size_t len) {
    {
        std::lock_guard<std::mutex> lk(qmu_);
        inQ_.emplace(data, data + len);
    }
    qcv_.notify_one();
}

void DtlsSrtp::flushOutbound() {
    char buf[4096];
    int n;
    while ((n = BIO_read(wbio_, buf, sizeof(buf))) > 0) {
        if (out_) out_(reinterpret_cast<std::uint8_t*>(buf), static_cast<std::size_t>(n));
    }
}

void DtlsSrtp::run() {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::seconds(15);
    bool fatal = false;

    while (!stop_.load() && !SSL_is_init_finished(ssl_) && clock::now() < deadline) {
        const int r = SSL_do_handshake(ssl_);
        flushOutbound();
        if (SSL_is_init_finished(ssl_)) break;

        const int err = SSL_get_error(ssl_, r);
        if (err == SSL_ERROR_WANT_READ) {
            // Wait for an inbound datagram or the DTLS retransmit timeout.
            timeval tv{};
            const int hasTimeout = DTLSv1_get_timeout(ssl_, &tv);
            const auto waitMs = hasTimeout
                                    ? std::chrono::milliseconds(tv.tv_sec * 1000 + tv.tv_usec / 1000)
                                    : std::chrono::milliseconds(1000);
            std::vector<std::uint8_t> dg;
            {
                std::unique_lock<std::mutex> lk(qmu_);
                if (inQ_.empty()) qcv_.wait_for(lk, waitMs);
                if (!inQ_.empty()) {
                    dg = std::move(inQ_.front());
                    inQ_.pop();
                }
            }
            if (stop_.load()) break;
            if (!dg.empty()) BIO_write(rbio_, dg.data(), static_cast<int>(dg.size()));
            else if (hasTimeout) DTLSv1_handle_timeout(ssl_);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            flushOutbound();
        } else {
            fatal = true;
            break;
        }
    }

    const bool ok = !fatal && SSL_is_init_finished(ssl_) && verifyPeerFingerprint() &&
                    deriveSrtpKeys();
    if (ok) ready_.store(true, std::memory_order_release);
    if (done_) done_(ok);
}

bool DtlsSrtp::verifyPeerFingerprint() {
    X509* peer = SSL_get1_peer_certificate(ssl_);
    if (!peer) return false;
    const std::string got = fingerprintOf(peer);
    X509_free(peer);
    if (remoteFp_.empty()) return false;  // refuse if we were never told the fp
    return normalizeFp(got) == normalizeFp(remoteFp_);
}

bool DtlsSrtp::deriveSrtpKeys() {
    unsigned char material[2 * kMasterLen];
    if (SSL_export_keying_material(ssl_, material, sizeof(material), "EXTRACTOR-dtls_srtp",
                                   19, nullptr, 0, 0) != 1) {
        return false;
    }
    // RFC 5764 layout: client_key | server_key | client_salt | server_salt.
    const unsigned char* clientKey = material;
    const unsigned char* serverKey = material + kKeyLen;
    const unsigned char* clientSalt = material + 2 * kKeyLen;
    const unsigned char* serverSalt = material + 2 * kKeyLen + kSaltLen;

    unsigned char clientMaster[kMasterLen], serverMaster[kMasterLen];
    std::memcpy(clientMaster, clientKey, kKeyLen);
    std::memcpy(clientMaster + kKeyLen, clientSalt, kSaltLen);
    std::memcpy(serverMaster, serverKey, kKeyLen);
    std::memcpy(serverMaster + kKeyLen, serverSalt, kSaltLen);

    unsigned char* outKey = (role_ == Role::Client) ? clientMaster : serverMaster;
    unsigned char* inKey = (role_ == Role::Client) ? serverMaster : clientMaster;

    auto makeSession = [](srtp_ctx_t_** sess, unsigned char* key, srtp_ssrc_type_t type) {
        srtp_policy_t pol;
        std::memset(&pol, 0, sizeof(pol));
        srtp_crypto_policy_set_rtp_default(&pol.rtp);
        srtp_crypto_policy_set_rtcp_default(&pol.rtcp);
        pol.ssrc.type = type;
        pol.ssrc.value = 0;
        pol.key = key;
        pol.next = nullptr;
        return srtp_create(reinterpret_cast<srtp_t*>(sess), &pol) == srtp_err_status_ok;
    };
    return makeSession(&srtpOut_, outKey, ssrc_any_outbound) &&
           makeSession(&srtpIn_, inKey, ssrc_any_inbound);
}

bool DtlsSrtp::protect(std::vector<std::uint8_t>& pkt) {
    std::lock_guard<std::mutex> lk(outMu_);
    if (!srtpOut_) return false;
    int len = static_cast<int>(pkt.size());
    pkt.resize(pkt.size() + SRTP_MAX_TRAILER_LEN + 4);
    const srtp_err_status_t st = srtp_protect(reinterpret_cast<srtp_t>(srtpOut_), pkt.data(), &len);
    if (st != srtp_err_status_ok) return false;
    pkt.resize(static_cast<std::size_t>(len));
    return true;
}

bool DtlsSrtp::unprotect(std::vector<std::uint8_t>& pkt) {
    std::lock_guard<std::mutex> lk(inMu_);
    if (!srtpIn_) return false;
    int len = static_cast<int>(pkt.size());
    const srtp_err_status_t st = srtp_unprotect(reinterpret_cast<srtp_t>(srtpIn_), pkt.data(), &len);
    if (st != srtp_err_status_ok) return false;
    pkt.resize(static_cast<std::size_t>(len));
    return true;
}

void DtlsSrtp::stop() {
    stop_.store(true);
    qcv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (srtpOut_) { srtp_dealloc(reinterpret_cast<srtp_t>(srtpOut_)); srtpOut_ = nullptr; }
    if (srtpIn_) { srtp_dealloc(reinterpret_cast<srtp_t>(srtpIn_)); srtpIn_ = nullptr; }
    if (ssl_) { SSL_free(ssl_); ssl_ = nullptr; }  // frees the BIOs too
}

}  // namespace vc::signaling
