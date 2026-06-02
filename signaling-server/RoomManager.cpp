#include "RoomManager.h"

#include <utility>

#include <nlohmann/json.hpp>

namespace vc::signaling {

using nlohmann::json;

RoomManager::RoomManager(SendFn send) : send_(std::move(send)) {}

RoomManager::Room* RoomManager::roomOf(ConnectionHandle conn, ParticipantId* outId) {
    auto it = connRoom_.find(conn);
    if (it == connRoom_.end()) return nullptr;
    auto roomIt = rooms_.find(it->second);
    if (roomIt == rooms_.end()) return nullptr;
    if (outId) {
        *outId = 0;
        for (const auto& p : roomIt->second.participants) {
            if (p.conn == conn) {
                *outId = p.id;
                break;
            }
        }
    }
    return &roomIt->second;
}

Result<ParticipantId, Error> RoomManager::join(std::string_view room,
                                               ConnectionHandle conn) {
    if (room.empty() || conn == nullptr) {
        return fail(Error::InvalidArgument);
    }
    // A connection may only live in one room at a time; drop any prior binding.
    if (connRoom_.count(conn)) {
        (void)leave(conn);
    }

    std::string roomName(room);
    Room& r = rooms_[roomName];
    if (r.name.empty()) r.name = roomName;

    if (r.participants.size() >= kMaxParticipants) {
        // Don't leave an empty room dangling if we just created it.
        if (r.participants.empty()) rooms_.erase(roomName);
        return fail(Error::ResourceExhausted);
    }

    const ParticipantId id = nextId_++;
    r.participants.push_back(Participant{id, conn});
    connRoom_[conn] = roomName;

    // Tell the new peer its id and who is already here.
    json peers = json::array();
    for (const auto& p : r.participants) {
        if (p.id != id) peers.push_back(p.id);
    }
    json welcome = {
        {"type", "joined"},
        {"room", roomName},
        {"id", id},
        {"peers", peers},
    };
    send_(conn, welcome.dump());

    // Notify the incumbents that a new peer arrived.
    json notice = {
        {"type", "peer-joined"},
        {"room", roomName},
        {"from", id},
    };
    const std::string noticeText = notice.dump();
    for (const auto& p : r.participants) {
        if (p.conn != conn) send_(p.conn, noticeText);
    }

    return id;
}

Status RoomManager::leave(ConnectionHandle conn) {
    auto it = connRoom_.find(conn);
    if (it == connRoom_.end()) return ok();  // unknown connection: no-op

    const std::string roomName = it->second;
    connRoom_.erase(it);

    auto roomIt = rooms_.find(roomName);
    if (roomIt == rooms_.end()) return ok();
    Room& r = roomIt->second;

    ParticipantId leavingId = 0;
    for (auto pIt = r.participants.begin(); pIt != r.participants.end(); ++pIt) {
        if (pIt->conn == conn) {
            leavingId = pIt->id;
            r.participants.erase(pIt);
            break;
        }
    }

    if (leavingId != 0) {
        json notice = {
            {"type", "peer-left"},
            {"room", roomName},
            {"from", leavingId},
        };
        const std::string noticeText = notice.dump();
        for (const auto& p : r.participants) {
            send_(p.conn, noticeText);
        }
    }

    if (r.participants.empty()) rooms_.erase(roomIt);
    return ok();
}

Status RoomManager::relay(ConnectionHandle conn, std::string_view type,
                          ParticipantId to, std::string_view rawJson) {
    if (type != "offer" && type != "answer" && type != "ice") {
        return fail(Error::Protocol);
    }

    ParticipantId fromId = 0;
    Room* r = roomOf(conn, &fromId);
    if (r == nullptr || fromId == 0) {
        return fail(Error::NotFound);
    }

    if (to != 0) {
        // Directed relay to a specific peer in the same room.
        for (const auto& p : r->participants) {
            if (p.id == to) {
                send_(p.conn, rawJson);
                return ok();
            }
        }
        return fail(Error::NotFound);
    }

    // Broadcast to everyone in the room except the sender.
    for (const auto& p : r->participants) {
        if (p.conn != conn) send_(p.conn, rawJson);
    }
    return ok();
}

std::size_t RoomManager::participantCount(std::string_view room) const {
    auto it = rooms_.find(std::string(room));
    return it == rooms_.end() ? 0 : it->second.participants.size();
}

ParticipantId RoomManager::idFor(ConnectionHandle conn) const {
    auto it = connRoom_.find(conn);
    if (it == connRoom_.end()) return 0;
    auto roomIt = rooms_.find(it->second);
    if (roomIt == rooms_.end()) return 0;
    for (const auto& p : roomIt->second.participants) {
        if (p.conn == conn) return p.id;
    }
    return 0;
}

}  // namespace vc::signaling
