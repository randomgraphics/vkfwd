#pragma once

#include "generated/command/vkFreeMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map_manager.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::FreeMemory> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_pack_enabled            = true;
    static constexpr bool after_response_unpack_enabled = false;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_pack(const ::vkfwd::generated::commands::vkFreeMemory::Command::Parameters & parameters) {
        // vkFreeMemory remains transport-deferrable, but future source-side maps
        // must stop seeing this handle immediately after the free call has been
        // accepted into this thread's serialized request stream.
        ::vkfwd::MemoryMapForwarder::instance().forget_allocation(parameters.memory);
    }

    template<class Parameters, class Response>
    static constexpr void after_response_unpack(const Parameters &, Response &) noexcept {}
};

} // namespace vkfwd::forwarder::manual
