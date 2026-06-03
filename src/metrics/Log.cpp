#include "metrics/Log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace vc::log {

void init(spdlog::level::level_enum level) {
    // [time] [level] [thread] message (source)
    auto logger = spdlog::stdout_color_mt("vc");
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [t:%t] %v");
    spdlog::set_level(level);
    spdlog::flush_on(spdlog::level::warn);
}

void setLevel(spdlog::level::level_enum level) { spdlog::set_level(level); }

} // namespace vc::log
