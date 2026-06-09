#pragma once

#include "command_stream.hpp"
#include "generated/receiver_hooks.hpp"
#include "logging.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::FlushMappedMemoryRanges> {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = false;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = true;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}

    static bool replace_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                 ::vkfwd::receiver::ReplayContext & replay_context) {
        (void) request_stream;
        (void) request_range;
        (void) response_stream;
        (void) replay_context;
        // The standard generated vkFlushMappedMemoryRanges command lacks the
        // per-range byte payload required to make host writes visible on the
        // receiver side. The public Vulkan entry point must use
        // manual::CommandId::MemoryFlush instead.
        VKFWD_LOG_ERROR("vkfwd receiver: standard generated vkFlushMappedMemoryRanges command is disabled; use manual::CommandId::MemoryFlush");
        return false;
    }
};

} // namespace vkfwd::receiver::manual
