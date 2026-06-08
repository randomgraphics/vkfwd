#pragma once

#include "generated/command/vkCreateDevice.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateDevice> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(const ::vkfwd::generated::commands::vkCreateDevice::Command::Parameters & parameters,
                                      ::vkfwd::generated::commands::vkCreateDevice::Command::Response &         response) {
        if (response.return_value != VK_SUCCESS) { return; }
        // The entry point writes *pDevice from *response.pDevice only after this
        // hook returns; read the response side so we capture the receiver-issued
        // handle, matching how vkAllocateMemoryForwarderHook reads *response.pMemory.
        if (!response.pDevice || *response.pDevice == VK_NULL_HANDLE) { return; }

        // Allocate-time code resolves memoryTypeIndex against properties keyed
        // by VkPhysicalDevice. vkAllocateMemory only gives us VkDevice, so the
        // forwarder must remember which physical device produced each logical
        // device the moment vkCreateDevice succeeds.
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_device(*response.pDevice, parameters.physicalDevice);
    }
};

} // namespace vkfwd::forwarder::manual
