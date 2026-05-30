#pragma once

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

#include <string_view>

namespace vkfwd::log {

spdlog::logger * default_logger();
spdlog::logger * logger(std::string_view name);

} // namespace vkfwd::log

#ifndef VKFWD_ACTIVE_LOGGER
    #define VKFWD_ACTIVE_LOGGER() ::vkfwd::log::default_logger()
#endif

#ifdef __FILE_NAME__
    #define VKFWD_LOG_SOURCE_FILE __FILE_NAME__
#else
    #define VKFWD_LOG_SOURCE_FILE __FILE__
#endif

#define VKFWD_LOG_STRINGIFY_DETAIL(value) #value
#define VKFWD_LOG_STRINGIFY(value)        VKFWD_LOG_STRINGIFY_DETAIL(value)

// The level check must live in the macro body so disabled log sites do not
// evaluate formatting arguments. Translation units may redirect subsequent log
// calls by undefining VKFWD_ACTIVE_LOGGER and defining it to another logger
// expression after this header has been included.
#define VKFWD_LOG_AT(level, method, ...)                                                                                                         \
    do {                                                                                                                                         \
        auto * vkfwd_active_logger = VKFWD_ACTIVE_LOGGER();                                                                                      \
        if (vkfwd_active_logger != nullptr && vkfwd_active_logger->should_log(level)) [[unlikely]] { vkfwd_active_logger->method(__VA_ARGS__); } \
    } while (false)

// Warning and error locations are intentionally injected by the macro instead
// of the shared formatter: debug/info logs stay compact, while severe messages
// still name the call site that observed the failure.
#define VKFWD_LOG_AT_SOURCE(level, method, ...)                                                                        \
    do {                                                                                                               \
        auto * vkfwd_active_logger = VKFWD_ACTIVE_LOGGER();                                                            \
        if (vkfwd_active_logger != nullptr && vkfwd_active_logger->should_log(level)) [[unlikely]] {                   \
            vkfwd_active_logger->method("[" VKFWD_LOG_SOURCE_FILE ":" VKFWD_LOG_STRINGIFY(__LINE__) "] " __VA_ARGS__); \
        }                                                                                                              \
    } while (false)

#define VKFWD_LOG_TRACE(...)    VKFWD_LOG_AT(::spdlog::level::trace, trace, __VA_ARGS__)
#define VKFWD_LOG_DEBUG(...)    VKFWD_LOG_AT(::spdlog::level::debug, debug, __VA_ARGS__)
#define VKFWD_LOG_INFO(...)     VKFWD_LOG_AT(::spdlog::level::info, info, __VA_ARGS__)
#define VKFWD_LOG_WARN(...)     VKFWD_LOG_AT_SOURCE(::spdlog::level::warn, warn, __VA_ARGS__)
#define VKFWD_LOG_ERROR(...)    VKFWD_LOG_AT_SOURCE(::spdlog::level::err, error, __VA_ARGS__)
#define VKFWD_LOG_CRITICAL(...) VKFWD_LOG_AT_SOURCE(::spdlog::level::critical, critical, __VA_ARGS__)
