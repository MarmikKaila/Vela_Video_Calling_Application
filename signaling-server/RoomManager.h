#pragma once

// RoomManager — server-side registry of signaling rooms.
//
// A "room" is a small group (capped at kMaxParticipants) of peers that want to
// negotiate WebRTC-style media sessions with each other. The signaling server
// itself never interprets media; it only routes JSON control messages
// (SDP offer/answer and ICE candidates) between the participants of a room.
//
// THREADING MODEL
// ----------------
// uWebSockets runs every socket callback on a single, thread-local event loop.
// All RoomManager mutations (join/leave/relay) are therefore expected to be
// invoked from that one loop thread, so the class needs NO internal locking.
// This is a deliberate, documented design choice — see the assertions and the
// note on each public method. If you ever drive RoomManager from multiple
// threads you must add external synchronisation.
//
// The class is transport-agnostic: it stores an opaque connection handle
// (void*) per participant and hands it back to a caller-supplied "send"
// function. main.cpp binds that handle to a uWS::WebSocket* and the send
// function to ws->send(...). This keeps RoomManager unit-testable without a
// live socket and free of any uWebSockets include.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/Error.h"

namespace vc::signaling {

// Maximum number of participants allowed in a single room.
inline constexpr std::size_t kMaxParticipants = 8;

// Opaque, non-owning handle to a participant's transport connection. The
// RoomManager never dereferences it; it only stores it and passes it to the
// SendFn supplied by the owner of the transport.
using ConnectionHandle = void*;

// Stable identifier handed to each participant on join. Unique within a room.
using ParticipantId = std::uint64_t;

// How RoomManager pushes a serialized message out over a participant's
// transport. `payload` is a complete JSON text frame. The implementation
// (main.cpp) maps `conn` back to its WebSocket and calls send().
using SendFn = std::function<void(ConnectionHandle conn, std::string_view payload)>;

// One participant inside a room.
struct Participant {
    ParticipantId id = 0;
    ConnectionHandle conn = nullptr;
};

// Registry of rooms and the participants within them. Owns no transport
// resources; lifetime is independent of any socket.
class RoomManager {
public:
    // `send` is how the manager delivers relayed/notification frames. Must be
    // non-null. Stored by value (std::function), so it may capture state such
    // as a pointer back to the uWS app.
    explicit RoomManager(SendFn send);

    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    // Add `conn` to `room`, creating the room if needed. Returns the freshly
    // assigned ParticipantId on success. Fails with:
    //   ResourceExhausted — room already holds kMaxParticipants peers
    //   InvalidArgument   — empty room name or null connection
    // On success the new peer is told its id (a "joined" message echoing
    // {type:"joined", room, id, peers:[...]}) and every existing peer is
    // notified ({type:"peer-joined", room, from:<newId>}).
    [[nodiscard]] Result<ParticipantId, Error> join(std::string_view room,
                                                     ConnectionHandle conn);

    // Remove the participant identified by `conn` from whichever room it is in.
    // Surviving peers receive {type:"peer-left", room, from:<id>}. Removing an
    // unknown connection is a no-op (returns ok) so it is safe to call from a
    // socket-close handler unconditionally. Empties of the room are pruned.
    Status leave(ConnectionHandle conn);

    // Relay an already-parsed control message originating from `conn`.
    //   type   — "offer" | "answer" | "ice" (anything else is rejected)
    //   to     — target ParticipantId, or 0 to broadcast to all other peers
    //   rawJson— the full JSON text to forward verbatim (with `from` filled in
    //            by the caller). RoomManager only decides the recipient set.
    // Fails with NotFound if `conn` is not in any room, or if a specific `to`
    // is not present in the sender's room. Protocol for unknown `type`.
    Status relay(ConnectionHandle conn, std::string_view type, ParticipantId to,
                 std::string_view rawJson);

    // --- Introspection (mainly for tests / diagnostics) --------------------

    // Number of live rooms.
    [[nodiscard]] std::size_t roomCount() const noexcept { return rooms_.size(); }

    // Number of participants in `room`, or 0 if the room does not exist.
    [[nodiscard]] std::size_t participantCount(std::string_view room) const;

    // The ParticipantId bound to `conn`, or 0 if the connection is unknown.
    [[nodiscard]] ParticipantId idFor(ConnectionHandle conn) const;

private:
    struct Room {
        std::string name;
        std::vector<Participant> participants;
    };

    // Locate the room a connection belongs to, plus the participant's id.
    // Returns nullptr if the connection is not registered anywhere.
    Room* roomOf(ConnectionHandle conn, ParticipantId* outId);

    SendFn send_;
    ParticipantId nextId_ = 1;  // 0 is reserved as "broadcast"/"unknown".
    // Keyed by room name. node-stable so we may hold Room* across map ops that
    // don't touch this node.
    std::unordered_map<std::string, Room> rooms_;
    // Fast reverse lookup: connection -> room name. Kept in sync with rooms_.
    std::unordered_map<ConnectionHandle, std::string> connRoom_;
};

}  // namespace vc::signaling
