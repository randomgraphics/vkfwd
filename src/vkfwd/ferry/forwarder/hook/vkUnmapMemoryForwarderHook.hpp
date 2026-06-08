#pragma once

#include "generated/command/vkUnmapMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/manager.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::UnmapMemory> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_pack_enabled            = true;
    static constexpr bool after_response_unpack_enabled = false;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_pack(const ::vkfwd::generated::commands::vkUnmapMemory::Command::Parameters & parameters) {
        // Transitional phase-0 hook: the full implementation will route the
        // public vkUnmapMemory entry point through manual::CommandId::MemoryUnmap.
        // There is no staging to flush while map still fails.
        ::vkfwd::MemoryMapForwarder::instance().custom_vkUnmapMemory_entry(parameters.device, parameters.memory);
    }

    template<class Parameters, class Response>
    static constexpr void after_response_unpack(const Parameters &, Response &) noexcept {}
};

} // namespace vkfwd::forwarder::manual
