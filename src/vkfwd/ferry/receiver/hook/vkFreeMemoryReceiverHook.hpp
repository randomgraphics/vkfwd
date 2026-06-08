#pragma once

#include "generated/command/vkFreeMemory.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::FreeMemory> {
    // The endpoint stub reads every *_enabled flag explicitly, so a specialization
    // must declare them all — even the ones that just inherit the default false.
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = true;
    static constexpr bool after_call_enabled           = false;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }

    // Erase first, then the entry stub calls real Vulkan vkFreeMemory. before_call
    // (not after_call) closes the window where a stale lookup could resolve a
    // doomed handle.
    //
    // Defense-in-depth: also drop any receiver-side memory-map allocation
    // record for this handle. Phase 1's unmap_endpoint already erases on a
    // well-behaved app's vkUnmapMemory before vkFreeMemory, but an app that
    // calls vkFreeMemory while the memory is still mapped (Vulkan spec calls
    // this undefined behavior, but real apps occasionally do it) would
    // otherwise leave a stale ReceiverAllocation pointing at a freed receiver
    // mapped pointer. Forgetting here keeps that case from biting us.
    static void before_call(const ::vkfwd::generated::commands::vkFreeMemory::Command::Parameters & parameters,
                            ::vkfwd::receiver::ReplayContext &                                      replay_context) {
        replay_context.source_to_receiver_memory.erase(parameters.memory);
        replay_context.memoryMap.forget_allocation(parameters.memory);
    }
};

} // namespace vkfwd::receiver::manual
