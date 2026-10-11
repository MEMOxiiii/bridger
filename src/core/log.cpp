#include <bridger/core/log.hpp>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>

namespace bridger::log {
namespace {

spdlog::logger& logger() {
    // Logs go to stderr so stdout stays clean for machine-readable CLI output.
    static const std::shared_ptr<spdlog::logger> instance = [] {
        auto l = spdlog::stderr_color_mt("bridger");
        l->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
        l->set_level(spdlog::level::info);
        return l;
    }();
    return *instance;
}

spdlog::level::level_enum to_spdlog(Level level) {
    switch (level) {
    case Level::Trace: return spdlog::level::trace;
    case Level::Debug: return spdlog::level::debug;
    case Level::Info: return spdlog::level::info;
    case Level::Warn: return spdlog::level::warn;
    case Level::Error: return spdlog::level::err;
    case Level::Off: return spdlog::level::off;
    }
    return spdlog::level::info;
}

} // namespace

void set_level(Level level) { logger().set_level(to_spdlog(level)); }

void write(Level level, std::string_view message) { logger().log(to_spdlog(level), message); }

} // namespace bridger::log
