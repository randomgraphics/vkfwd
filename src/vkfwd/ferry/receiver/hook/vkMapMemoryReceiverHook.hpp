#pragma once

#include "command_stream.hpp"
#include "generated/receiver_hooks.hpp"
#include "logging.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::MapMemory> {
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

    static bool replace_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
        // Standard generated vkMapMemory cannot preserve vkfwd's cross-process
        // staging invariant: the receiver mapped pointer must never be packed
        // into a generated Vulkan response. The public Vulkan entry point must
        // use manual::CommandId::MemoryMap instead.
        VKFWD_LOG_ERROR("vkfwd receiver: standard generated vkMapMemory command is disabled; use manual::CommandId::MemoryMap");
        return false;
    }
};

} // namespace vkfwd::receiver::manual
