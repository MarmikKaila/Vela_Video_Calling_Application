#include "NATTraversal.h"

#include <cstdlib>
#include <cstring>
#include <utility>

#include <juice/juice.h>

namespace vc::signaling {

std::string_view to_string(IceState s) noexcept {
    switch (s) {
        case IceState::Disconnected: return "Disconnected";
        case IceState::Gathering:    return "Gathering";
        case IceState::Connecting:   return "Connecting";
        case IceState::Connected:    return "Connected";
        case IceState::Completed:    return "Completed";
        case IceState::Failed:       return "Failed";
    }
    return "Unknown";
}

namespace {
IceState fromJuice(juice_state_t s) {
    switch (s) {
        case JUICE_STATE_DISCONNECTED: return IceState::Disconnected;
        case JUICE_STATE_GATHERING:    return IceState::Gathering;
        case JUICE_STATE_CONNECTING:   return IceState::Connecting;
        case JUICE_STATE_CONNECTED:    return IceState::Connected;
        case JUICE_STATE_COMPLETED:    return IceState::Completed;
        case JUICE_STATE_FAILED:       return IceState::Failed;
    }
    return IceState::Disconnected;
}
}  // namespace

NATTraversal::NATTraversal() = default;

NATTraversal::~NATTraversal() {
    if (agent_) {
        juice_destroy(agent_);
        agent_ = nullptr;
    }
}

void NATTraversal::handleStateChanged(int juiceState) {
    const IceState s = fromJuice(static_cast<juice_state_t>(juiceState));
    state_.store(s);
    if (cbs_.onStateChanged) cbs_.onStateChanged(s);
}

void NATTraversal::handleCandidate(const char* sdp) {
    if (cbs_.onLocalCandidate && sdp) cbs_.onLocalCandidate(sdp);
}

void NATTraversal::handleGatheringDone() {
    if (cbs_.onGatheringDone) cbs_.onGatheringDone();
}

void NATTraversal::handleRecv(const char* data, std::size_t size) {
    if (cbs_.onData)
        cbs_.onData(reinterpret_cast<const std::uint8_t*>(data), size);
}

namespace {
// File-local trampolines whose signatures match libjuice's callback typedefs
// exactly, so they can be assigned to juice_config_t fields without UB. Each
// recovers the NATTraversal from user_ptr and forwards to a handler.
void cbStateChanged(juice_agent_t*, juice_state_t state, void* user) {
    static_cast<NATTraversal*>(user)->handleStateChanged(static_cast<int>(state));
}
void cbCandidate(juice_agent_t*, const char* sdp, void* user) {
    static_cast<NATTraversal*>(user)->handleCandidate(sdp);
}
void cbGatheringDone(juice_agent_t*, void* user) {
    static_cast<NATTraversal*>(user)->handleGatheringDone();
}
void cbRecv(juice_agent_t*, const char* data, size_t size, void* user) {
    static_cast<NATTraversal*>(user)->handleRecv(data, size);
}
}  // namespace

Status NATTraversal::initialize(const NATConfig& cfg, NATCallbacks cbs) {
    if (agent_) return fail(Error::AlreadyRunning);
    cfg_ = cfg;
    cbs_ = std::move(cbs);
    stunHostStorage_ = cfg_.stunHost;

    juice_config_t jc;
    std::memset(&jc, 0, sizeof(jc));
    jc.concurrency_mode = JUICE_CONCURRENCY_MODE_POLL;
    // Host candidates are always gathered by libjuice regardless of STUN, so
    // leaving the STUN fields populated still yields a working offline agent
    // (it just additionally tries the reflexive lookup). An empty stunHost
    // disables STUN entirely.
    if (!stunHostStorage_.empty()) {
        jc.stun_server_host = stunHostStorage_.c_str();
        jc.stun_server_port = cfg_.stunPort;
    }
    // TURN relays. cfg_ is our own member copy, so the strings outlive the agent;
    // turnStorage_ holds the juice structs (pointers into cfg_) for its lifetime.
    if (!cfg_.turnServers.empty()) {
        turnStorage_.clear();
        turnStorage_.reserve(cfg_.turnServers.size());
        for (const auto& t : cfg_.turnServers) {
            juice_turn_server_t js;
            std::memset(&js, 0, sizeof(js));
            js.host = t.host.c_str();
            js.username = t.username.c_str();
            js.password = t.password.c_str();
            js.port = t.port;
            turnStorage_.push_back(js);
        }
        jc.turn_servers = turnStorage_.data();
        jc.turn_servers_count = static_cast<int>(turnStorage_.size());
    }
    jc.cb_state_changed = &cbStateChanged;
    jc.cb_candidate = &cbCandidate;
    jc.cb_gathering_done = &cbGatheringDone;
    jc.cb_recv = &cbRecv;
    jc.user_ptr = this;

    agent_ = juice_create(&jc);
    if (!agent_) return fail(Error::NetworkError);
    return ok();
}

Status NATTraversal::startGathering() {
    if (!agent_) return fail(Error::NotInitialized);
    if (juice_gather_candidates(agent_) != JUICE_ERR_SUCCESS) {
        return fail(Error::NetworkError);
    }
    return ok();
}

Result<std::string, Error> NATTraversal::localDescription() const {
    if (!agent_) return fail(Error::NotInitialized);
    char buf[JUICE_MAX_SDP_STRING_LEN];
    if (juice_get_local_description(agent_, buf, sizeof(buf)) != JUICE_ERR_SUCCESS) {
        return fail(Error::NetworkError);
    }
    return std::string(buf);
}

Status NATTraversal::setRemoteDescription(std::string_view sdp) {
    if (!agent_) return fail(Error::NotInitialized);
    const std::string s(sdp);
    if (juice_set_remote_description(agent_, s.c_str()) != JUICE_ERR_SUCCESS) {
        return fail(Error::Protocol);
    }
    return ok();
}

Status NATTraversal::addRemoteCandidate(std::string_view candidate) {
    if (!agent_) return fail(Error::NotInitialized);
    const std::string s(candidate);
    if (juice_add_remote_candidate(agent_, s.c_str()) != JUICE_ERR_SUCCESS) {
        return fail(Error::Protocol);
    }
    return ok();
}

Status NATTraversal::setRemoteGatheringDone() {
    if (!agent_) return fail(Error::NotInitialized);
    if (juice_set_remote_gathering_done(agent_) != JUICE_ERR_SUCCESS) {
        return fail(Error::NetworkError);
    }
    return ok();
}

Status NATTraversal::send(const std::uint8_t* data, std::size_t len) {
    if (!agent_) return fail(Error::NotInitialized);
    if (juice_send(agent_, reinterpret_cast<const char*>(data), len) !=
        JUICE_ERR_SUCCESS) {
        return fail(Error::NetworkError);
    }
    return ok();
}

NATConfig iceConfigFromEnv() {
    NATConfig cfg;  // defaults to Google STUN, no TURN
    auto splitHostPort = [](const std::string& s, std::uint16_t defPort,
                            std::string& host, std::uint16_t& port) {
        const auto colon = s.rfind(':');
        if (colon != std::string::npos) {
            host = s.substr(0, colon);
            const int p = std::atoi(s.substr(colon + 1).c_str());
            port = (p > 0 && p <= 65535) ? static_cast<std::uint16_t>(p) : defPort;
        } else {
            host = s;
            port = defPort;
        }
    };
    if (const char* stun = std::getenv("VC_STUN")) {
        const std::string s = stun;
        if (s.empty() || s == "off") cfg.stunHost.clear();  // LAN-only
        else splitHostPort(s, 19302, cfg.stunHost, cfg.stunPort);
    }
    if (const char* turn = std::getenv("VC_TURN")) {
        if (*turn) {
            TurnServer ts;
            splitHostPort(turn, 3478, ts.host, ts.port);
            if (const char* u = std::getenv("VC_TURN_USER")) ts.username = u;
            if (const char* p = std::getenv("VC_TURN_PASS")) ts.password = p;
            cfg.turnServers.push_back(std::move(ts));
        }
    }
    return cfg;
}

}  // namespace vc::signaling
