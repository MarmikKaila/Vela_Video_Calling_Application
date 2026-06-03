#pragma once

// Structured logging facade over spdlog.
//
// Modules log through the VC_* macros rather than touching spdlog directly, so
// the backend (sinks, format, async) can change in one place. Call vc::log::init
// once at startup; if you never call it, spdlog's default logger is used.
//
// The format includes a timestamp, level, thread id, and the source location of
// the call site, which is what you want when debugging a real-time media path
// across capture / encode / network threads.

#include <spdlog/spdlog.h>

namespace vc::log {

// Initialise the global logger with a coloured stdout sink and a format suited
// to multi-threaded media debugging. `level` sets the minimum severity emitted.
void init(spdlog::level::level_enum level = spdlog::level::info);

// Set the minimum severity at runtime.
void setLevel(spdlog::level::level_enum level);

} // namespace vc::log

// Thin macros. fmt-style: VC_INFO("rtt={}ms loss={}%", rtt, loss).
#define VC_TRACE(...) ::spdlog::trace(__VA_ARGS__)
#define VC_DEBUG(...) ::spdlog::debug(__VA_ARGS__)
#define VC_INFO(...)  ::spdlog::info(__VA_ARGS__)
#define VC_WARN(...)  ::spdlog::warn(__VA_ARGS__)
#define VC_ERROR(...) ::spdlog::error(__VA_ARGS__)
