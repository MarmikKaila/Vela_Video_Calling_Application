#include "SignalingClient.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace vc::signaling {
namespace {

using nlohmann::json;

// ---- Minimal SHA-1 (RFC 3174) for the WebSocket handshake -----------------
// Self-contained so we add no crypto dependency. Used only on the 16-byte
// nonce + GUID; not security-sensitive beyond the handshake itself.
class Sha1 {
public:
    Sha1() { reset(); }

    void update(const std::uint8_t* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            buffer_[bufferLen_++] = data[i];
            if (bufferLen_ == 64) {
                processBlock(buffer_.data());
                bufferLen_ = 0;
            }
            ++totalLen_;
        }
    }

    std::array<std::uint8_t, 20> digest() {
        std::uint64_t bitLen = totalLen_ * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0x00;
        while (bufferLen_ != 56) update(&zero, 1);
        for (int i = 7; i >= 0; --i) {
            std::uint8_t b = static_cast<std::uint8_t>((bitLen >> (i * 8)) & 0xff);
            update(&b, 1);
        }
        std::array<std::uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[i * 4 + 0] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xff);
            out[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xff);
            out[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xff);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xff);
        }
        return out;
    }

private:
    void reset() {
        h_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        bufferLen_ = 0;
        totalLen_ = 0;
    }

    static std::uint32_t rol(std::uint32_t v, int b) {
        return (v << b) | (v >> (32 - b));
    }

    void processBlock(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) | (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) | std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = tmp;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::array<std::uint32_t, 5> h_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferLen_ = 0;
    std::uint64_t totalLen_ = 0;
};

std::string base64(const std::uint8_t* data, std::size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < len; i += 3) {
        std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
    }
    if (i < len) {
        std::uint32_t n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? tbl[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// Blocking read of exactly n bytes; false on EOF/error.
bool readExact(int fd, std::uint8_t* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool writeAll(int fd, const std::uint8_t* buf, std::size_t n) {
    std::size_t sent = 0;
    while (sent < n) {
        ssize_t w = ::send(fd, buf + sent, n - sent, 0);
        if (w <= 0) return false;
        sent += static_cast<std::size_t>(w);
    }
    return true;
}

}  // namespace

SignalingClient::SignalingClient() = default;

SignalingClient::~SignalingClient() { disconnect(); }

void SignalingClient::setCallbacks(SignalingCallbacks cbs) { cbs_ = std::move(cbs); }

void SignalingClient::closeSocket() noexcept {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

Status SignalingClient::connect(const SignalingConfig& cfg) {
    if (connected_.load()) return fail(Error::AlreadyRunning);
    if (cfg.room.empty()) return fail(Error::InvalidArgument);
    cfg_ = cfg;
    stopping_.store(false);
    selfId_.store(0);

    // Resolve and connect (IPv4/IPv6 agnostic).
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(cfg.port);
    if (::getaddrinfo(cfg.host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
        return fail(Error::NetworkError);
    }
    int fd = -1;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) return fail(Error::NetworkError);
    fd_ = fd;
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (auto s = doHandshake(); !s) {
        closeSocket();
        return s;
    }

    connected_.store(true);
    recvThread_ = std::thread([this] { runRecvLoop(); });

    // Announce ourselves into the room.
    json join = {{"type", "join"}, {"room", cfg_.room}};
    if (auto s = sendTextFrame(join.dump()); !s) {
        disconnect();
        return s;
    }
    return ok();
}

Status SignalingClient::doHandshake() {
    // Generate a 16-byte random nonce, base64 it, send the upgrade request.
    std::array<std::uint8_t, 16> nonce{};
    std::random_device rd;
    for (auto& b : nonce) b = static_cast<std::uint8_t>(rd());
    const std::string key = base64(nonce.data(), nonce.size());

    std::string req;
    req += "GET " + cfg_.path + " HTTP/1.1\r\n";
    req += "Host: " + cfg_.host + ":" + std::to_string(cfg_.port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    req += "\r\n";
    if (!writeAll(fd_, reinterpret_cast<const std::uint8_t*>(req.data()), req.size())) {
        return fail(Error::NetworkError);
    }

    // Read the response headers up to the blank line. (Server replies before
    // any data frame, so a byte-at-a-time read until CRLFCRLF is safe.)
    std::string resp;
    std::uint8_t c;
    while (resp.find("\r\n\r\n") == std::string::npos) {
        if (resp.size() > 8192) return fail(Error::Protocol);
        if (!readExact(fd_, &c, 1)) return fail(Error::NetworkError);
        resp.push_back(static_cast<char>(c));
    }
    if (resp.find(" 101 ") == std::string::npos) return fail(Error::Protocol);

    // Verify Sec-WebSocket-Accept = base64(sha1(key + GUID)).
    const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(magic.data()), magic.size());
    auto d = sha.digest();
    const std::string expect = base64(d.data(), d.size());
    if (resp.find(expect) == std::string::npos) return fail(Error::Protocol);
    return ok();
}

Status SignalingClient::sendTextFrame(std::string_view text) {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (fd_ < 0) return fail(Error::NotInitialized);

    std::vector<std::uint8_t> frame;
    frame.push_back(0x81);  // FIN + opcode 0x1 (text)

    const std::size_t len = text.size();
    std::uint8_t maskBit = 0x80;  // client frames MUST be masked
    if (len < 126) {
        frame.push_back(static_cast<std::uint8_t>(maskBit | len));
    } else if (len <= 0xFFFF) {
        frame.push_back(maskBit | 126);
        frame.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
        frame.push_back(static_cast<std::uint8_t>(len & 0xff));
    } else {
        frame.push_back(maskBit | 127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<std::uint8_t>((len >> (i * 8)) & 0xff));
    }

    std::array<std::uint8_t, 4> mask{};
    std::random_device rd;
    for (auto& m : mask) m = static_cast<std::uint8_t>(rd());
    frame.insert(frame.end(), mask.begin(), mask.end());

    const auto* payload = reinterpret_cast<const std::uint8_t*>(text.data());
    for (std::size_t i = 0; i < len; ++i) {
        frame.push_back(payload[i] ^ mask[i % 4]);
    }

    if (!writeAll(fd_, frame.data(), frame.size())) return fail(Error::NetworkError);
    return ok();
}

void SignalingClient::runRecvLoop() {
    auto reportError = [this](Error e, const std::string& detail) {
        if (cbs_.onError && !stopping_.load()) cbs_.onError(e, detail);
    };

    while (!stopping_.load()) {
        std::uint8_t hdr[2];
        if (!readExact(fd_, hdr, 2)) break;

        const bool fin = (hdr[0] & 0x80) != 0;
        const std::uint8_t opcode = hdr[0] & 0x0f;
        const bool masked = (hdr[1] & 0x80) != 0;  // server frames are unmasked
        std::uint64_t len = hdr[1] & 0x7f;

        if (len == 126) {
            std::uint8_t ext[2];
            if (!readExact(fd_, ext, 2)) break;
            len = (std::uint64_t(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            std::uint8_t ext[8];
            if (!readExact(fd_, ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }

        std::array<std::uint8_t, 4> mask{};
        if (masked && !readExact(fd_, mask.data(), 4)) break;

        std::string payload;
        payload.resize(static_cast<std::size_t>(len));
        if (len > 0 &&
            !readExact(fd_, reinterpret_cast<std::uint8_t*>(payload.data()),
                       static_cast<std::size_t>(len))) {
            break;
        }
        if (masked) {
            for (std::size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];
        }

        if (opcode == 0x8) break;       // close
        if (opcode == 0x9) continue;    // ping (server pings are informational)
        if (opcode == 0xA) continue;    // pong
        if (!fin) {
            // Control protocol messages are tiny; we do not expect
            // fragmentation. Treat it as a protocol error.
            reportError(Error::Protocol, "unexpected fragmented frame");
            break;
        }
        if (opcode != 0x1) continue;    // ignore binary

        json j = json::parse(payload, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            reportError(Error::Protocol, "malformed server JSON");
            continue;
        }
        const std::string type = j.value("type", std::string{});
        const PeerId from = j.value("from", static_cast<PeerId>(0));

        if (type == "joined") {
            selfId_.store(j.value("id", static_cast<PeerId>(0)));
            if (cbs_.onJoined) cbs_.onJoined(selfId_.load());
            if (cbs_.onPeerJoined && j.contains("peers") && j["peers"].is_array()) {
                for (const auto& p : j["peers"]) {
                    cbs_.onPeerJoined(p.get<PeerId>());
                }
            }
        } else if (type == "peer-joined") {
            if (cbs_.onPeerJoined) cbs_.onPeerJoined(from);
        } else if (type == "peer-left") {
            if (cbs_.onPeerLeft) cbs_.onPeerLeft(from);
        } else if (type == "offer") {
            if (cbs_.onOffer)
                cbs_.onOffer(from, j.value("payload", std::string{}));
        } else if (type == "answer") {
            if (cbs_.onAnswer)
                cbs_.onAnswer(from, j.value("payload", std::string{}));
        } else if (type == "ice") {
            if (cbs_.onIceCandidate)
                cbs_.onIceCandidate(from, j.value("payload", std::string{}));
        } else if (type == "error") {
            reportError(Error::Protocol, j.value("reason", std::string{}));
        }
    }

    const bool wasStopping = stopping_.load();
    connected_.store(false);
    if (!wasStopping) reportError(Error::NetworkError, "connection closed");
}

void SignalingClient::disconnect() {
    if (stopping_.exchange(true)) {
        if (recvThread_.joinable() &&
            recvThread_.get_id() != std::this_thread::get_id()) {
            recvThread_.join();
        }
        return;
    }
    if (connected_.load()) {
        json leave = {{"type", "leave"}, {"room", cfg_.room}};
        (void)sendTextFrame(leave.dump());
    }
    closeSocket();  // unblocks the recv loop
    if (recvThread_.joinable() &&
        recvThread_.get_id() != std::this_thread::get_id()) {
        recvThread_.join();
    }
    connected_.store(false);
}

Status SignalingClient::sendMessage(std::string_view type, PeerId to,
                                    std::string_view payload) {
    if (!connected_.load()) return fail(Error::NotInitialized);
    json j = {
        {"type", std::string(type)},
        {"room", cfg_.room},
        {"payload", std::string(payload)},
    };
    if (to != 0) j["to"] = to;
    return sendTextFrame(j.dump());
}

Status SignalingClient::sendOffer(PeerId to, std::string_view sdp) {
    return sendMessage("offer", to, sdp);
}

Status SignalingClient::sendAnswer(PeerId to, std::string_view sdp) {
    return sendMessage("answer", to, sdp);
}

Status SignalingClient::sendIceCandidate(PeerId to, std::string_view candidate) {
    return sendMessage("ice", to, candidate);
}

}  // namespace vc::signaling
