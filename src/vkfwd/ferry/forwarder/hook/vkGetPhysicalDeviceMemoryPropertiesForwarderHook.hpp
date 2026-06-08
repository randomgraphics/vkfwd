#pragma once

#include "generated/command/vkGetPhysicalDeviceMemoryProperties.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::GetPhysicalDeviceMemoryProperties> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(const ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command::Parameters & parameters,
                                      ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command::Response &         response) {
        // The receiver-populated data lives in *response.pMemoryProperties; the
        // entry copies it back into *parameters.pMemoryProperties only after
        // this hook returns. Read from the response so the cache holds
        // receiver-filled values, not the app's uninitialized output buffer.
        if (!response.pMemoryProperties) { return; }
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_memory_properties(parameters.physicalDevice, *response.pMemoryProperties);
    }
};

} // namespace vkfwd::forwarder::manual
