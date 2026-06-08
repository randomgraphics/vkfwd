#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map_manager.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::AllocateMemory> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(const ::vkfwd::generated::commands::vkAllocateMemory::Command::Parameters & parameters,
                                      ::vkfwd::generated::commands::vkAllocateMemory::Command::Response &         response) {
        if (response.return_value != VK_SUCCESS || !parameters.pAllocateInfo || !response.pMemory || *response.pMemory == VK_NULL_HANDLE) { return; }

        // Allocation extent tracking belongs to the source-side map manager
        // because VK_WHOLE_SIZE must later be resolved against the same
        // caller-visible VkDeviceMemory handle that generated copy-back returns.
        ::vkfwd::MemoryMapForwarder::instance().record_allocation(parameters.device, *response.pMemory, parameters.pAllocateInfo->allocationSize);
    }
};

} // namespace vkfwd::forwarder::manual
