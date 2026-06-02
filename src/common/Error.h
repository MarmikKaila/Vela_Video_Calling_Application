#pragma once

// Project-wide error taxonomy. Modules may extend this with their own detail
// codes, but every fallible boundary should map onto one of these categories so
// the UI/logging layer can reason about failures uniformly.

#include <string_view>

#include "common/Result.h"

namespace vc {

enum class Error {
    Ok = 0,            // never returned via fail(); present for completeness
    NotFound,          // device, room, participant, codec not available
    InvalidArgument,   // caller passed something nonsensical
    NotInitialized,    // object used before open()/start()
    AlreadyRunning,    // start() called twice
    Unsupported,       // platform/format/codec not supported on this build
    DeviceError,       // OS-level capture/render failure
    CodecError,        // encode/decode failure
    NetworkError,      // socket / transport failure
    Timeout,           // operation exceeded its deadline
    ResourceExhausted, // pool empty, queue full, out of buffers
    Protocol,          // malformed SDP / RTP / signaling message
    Internal,          // invariant violation — a bug
};

[[nodiscard]] constexpr std::string_view to_string(Error e) noexcept {
    switch (e) {
        case Error::Ok:                return "Ok";
        case Error::NotFound:          return "NotFound";
        case Error::InvalidArgument:   return "InvalidArgument";
        case Error::NotInitialized:    return "NotInitialized";
        case Error::AlreadyRunning:    return "AlreadyRunning";
        case Error::Unsupported:       return "Unsupported";
        case Error::DeviceError:       return "DeviceError";
        case Error::CodecError:        return "CodecError";
        case Error::NetworkError:      return "NetworkError";
        case Error::Timeout:           return "Timeout";
        case Error::ResourceExhausted: return "ResourceExhausted";
        case Error::Protocol:          return "Protocol";
        case Error::Internal:          return "Internal";
    }
    return "Unknown";
}

// Success-or-error with no payload. `return ok();` / `return fail(Error::X);`
using Status = Result<Unit, Error>;

[[nodiscard]] inline Status ok() { return Status{unit}; }

} // namespace vc
