#pragma once

#include "generated/command/vkMapMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map_manager.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::MapMemory> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(const ::vkfwd::generated::commands::vkMapMemory::Command::Parameters & parameters,
                                      ::vkfwd::generated::commands::vkMapMemory::Command::Response &         response) {
        if (response.return_value != VK_SUCCESS) { return; }

        // Transitional phase-0 hook: the full implementation will route the
        // public vkMapMemory entry point through manual::CommandId::MemoryMap.
        // Until that custom command lands, fail before generated copy-back can
        // expose a receiver-process pointer to the source application.
        response.return_value = ::vkfwd::MemoryMapForwarder::instance().custom_vkMapMemory_entry(parameters.device, parameters.memory, parameters.offset,
                                                                                                 parameters.size, parameters.flags, parameters.ppData);
        response.ppData       = parameters.ppData;
    }
};

} // namespace vkfwd::forwarder::manual
