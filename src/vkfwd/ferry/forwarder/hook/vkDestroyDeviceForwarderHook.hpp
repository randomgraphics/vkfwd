#pragma once

#include "generated/command/vkDestroyDevice.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::DestroyDevice> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_pack_enabled            = true;
    static constexpr bool after_response_unpack_enabled = false;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    // Drop the device entry the moment the destroy command is accepted into the
    // request stream. A later vkAllocateMemory on the same handle must not be
    // able to resolve through the doomed device — doing this at after_pack
    // rather than after_response_unpack closes that window even before the
    // receiver has acknowledged the destroy.
    static void after_pack(const ::vkfwd::generated::commands::vkDestroyDevice::Command::Parameters & parameters) {
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().forget_device(parameters.device);
    }

    template<class Parameters, class Response>
    static constexpr void after_response_unpack(const Parameters &, Response &) noexcept {}
};

} // namespace vkfwd::forwarder::manual
