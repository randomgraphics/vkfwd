#include "logging.hpp"

#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <mutex>
#include <string>

namespace vkfwd::log {
namespace {

std::mutex & logger_mutex() {
    static std::mutex mutex;
    return mutex;
}

void configure_pattern_once() {
    static const bool configured = [] {
        // The logger name is vkfwd's tag. Keeping it in the pattern makes stdout
        // logs grep-friendly and matches spdlog's per-logger runtime level control.
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
        return true;
    }();
    (void) configured;
}

void load_env_levels_once() {
    static const bool loaded = [] {
        // SPDLOG_LEVEL supports entries such as "off,vkfwd.pack=debug". Loading
        // once before logger creation lets named loggers inherit configured
        // levels from spdlog's registry as they are registered.
        spdlog::cfg::load_env_levels();
        return true;
    }();
    (void) loaded;
}

} // namespace

spdlog::logger * logger(std::string_view name) {
    load_env_levels_once();
    configure_pattern_once();

    const std::string logger_name(name);
    std::lock_guard   lock(logger_mutex());
    if (auto existing = spdlog::get(logger_name)) { return existing.get(); }

    auto created = spdlog::stdout_color_mt(logger_name);
    return created.get();
}

spdlog::logger * default_logger() { return logger("vkfwd"); }

} // namespace vkfwd::log
