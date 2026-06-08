#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_type_registry.hpp"

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

        const auto resolved = ::vkfwd::memory_map::MemoryTypeRegistry::instance().resolve(parameters.device, parameters.pAllocateInfo->memoryTypeIndex);
        if (!resolved) {
            // Phase 0 has only the opportunistic cache. Vulkan does not require
            // the app to call property queries before allocation, so this is a
            // vkfwd classification miss rather than an app error. Phase 1 adds
            // manual::CommandId::QueryPhysicalDeviceMemoryInfo as the fallback.
            // The allocation is still valid on the receiver — only manager
            // bookkeeping is skipped, which downgrades vkMapMemory on this
            // handle to a visible VK_ERROR_FEATURE_NOT_PRESENT.
            VKFWD_LOG_ERROR("vkfwd: memory_type_registry has no entry for device={} memoryTypeIndex={}; vkAllocateMemory tracked record skipped",
                            static_cast<void *>(parameters.device), parameters.pAllocateInfo->memoryTypeIndex);
            return;
        }

        ::vkfwd::MemoryMapForwarder::instance().record_allocation(parameters.device, *response.pMemory, resolved->property_flags,
                                                                  parameters.pAllocateInfo->memoryTypeIndex, parameters.pAllocateInfo->allocationSize,
                                                                  resolved->non_coherent_atom_size, resolved->min_memory_map_alignment);
    }
};

} // namespace vkfwd::forwarder::manual
