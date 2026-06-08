#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_info_fallback.hpp"
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

        auto resolved = ::vkfwd::memory_map::MemoryTypeRegistry::instance().resolve(parameters.device, parameters.pAllocateInfo->memoryTypeIndex);
        if (!resolved) {
            // Phase 1 fallback: Vulkan does not require the app to call any
            // property queries before vkAllocateMemory, so the opportunistic
            // cache may legitimately miss on the first allocation. Ask the
            // receiver for the physical-device's memory properties + limits
            // synchronously, populate the registry, then retry resolve once.
            ::vkfwd::memory_map::request_memory_info_fallback(parameters.device);
            resolved = ::vkfwd::memory_map::MemoryTypeRegistry::instance().resolve(parameters.device, parameters.pAllocateInfo->memoryTypeIndex);
        }
        if (!resolved) {
            // Even after the fallback we cannot classify. The allocation is
            // still valid on the receiver — only manager bookkeeping is
            // skipped, which downgrades vkMapMemory on this handle to a
            // visible VK_ERROR_FEATURE_NOT_PRESENT. Common cause: no
            // vkCreateDevice was observed for this VkDevice, so we don't even
            // know which physical device to query.
            VKFWD_LOG_ERROR("vkfwd: memory_type_registry has no entry for device={} memoryTypeIndex={} even after fallback; tracked record skipped",
                            static_cast<void *>(parameters.device), parameters.pAllocateInfo->memoryTypeIndex);
            return;
        }

        ::vkfwd::MemoryMapForwarder::instance().record_allocation(parameters.device, *response.pMemory, resolved->property_flags,
                                                                  parameters.pAllocateInfo->memoryTypeIndex, parameters.pAllocateInfo->allocationSize,
                                                                  resolved->non_coherent_atom_size, resolved->min_memory_map_alignment);
    }
};

} // namespace vkfwd::forwarder::manual
