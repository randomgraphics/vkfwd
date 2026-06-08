#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::AllocateMemory> {
    // The endpoint stub reads every *_enabled flag explicitly, so a specialization
    // must declare them all — even the ones that just inherit the default false.
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = true;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }

    // Records the source-visible -> receiver-native VkDeviceMemory mapping so
    // future manual MemoryMap / MemoryUnmap chunks can translate the handle
    // before calling real Vulkan. Phase 1 runs source and receiver in the
    // same process via loopback_runtime — the receiver writes its handle back
    // into pMemory which the forwarder propagates to the app — so this map
    // ends up storing (h, h) pairs. Cross-process receivers that mint
    // distinct handles will populate the same map without any code change.
    static void after_call(const ::vkfwd::generated::commands::vkAllocateMemory::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkAllocateMemory::Command::Response &   response,
                           ::vkfwd::receiver::ReplayContext &                                          replay_context) {
        if (response.return_value != VK_SUCCESS || !parameters.pMemory || *parameters.pMemory == VK_NULL_HANDLE) { return; }
        replay_context.source_to_receiver_memory[*parameters.pMemory] = *parameters.pMemory;
    }
};

} // namespace vkfwd::receiver::manual
