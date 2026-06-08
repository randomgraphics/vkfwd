#pragma once

#include "generated/command/vkGetPhysicalDeviceProperties.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::GetPhysicalDeviceProperties> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(const ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command::Parameters & parameters,
                                      ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command::Response &         response) {
        // The receiver-populated data lives in *response.pProperties; the entry
        // copies it back into *parameters.pProperties only after this hook
        // returns. Read from the response so the cache holds receiver-filled
        // values, not the app's uninitialized output buffer. Both limits flow
        // into the same hook: nonCoherentAtomSize gates the alignment of
        // receiver-side flush/invalidate ranges, and minMemoryMapAlignment
        // gates the source-side mapped-pointer contract.
        if (!response.pProperties) { return; }
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_non_coherent_atom_size(parameters.physicalDevice, response.pProperties->limits.nonCoherentAtomSize);
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_min_memory_map_alignment(parameters.physicalDevice, response.pProperties->limits.minMemoryMapAlignment);
    }
};

} // namespace vkfwd::forwarder::manual
