#pragma once

#include "generated/command/vkCreateDevice.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateDevice> {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = true;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_call(Args &...) noexcept {}

    static void after_call(const ::vkfwd::generated::commands::vkCreateDevice::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkCreateDevice::Command::Response &   response,
                           ::vkfwd::receiver::ReplayContext &                                        replay_context) {
        // Device dispatch is receiver-owned state derived from the destination
        // device handle. Source-side forwarding must not provide or cache these
        // host function pointers.
        if (response.return_value == VK_SUCCESS && parameters.pDevice && *parameters.pDevice) {
            replay_context.dispatch.device.init(*parameters.pDevice, replay_context.dispatch.instance.get_device_proc_addr);
            // Same forwarder/receiver handle equivalence as vkAllocateMemoryReceiverHook:
            // future manual command chunks arrive carrying source handles and must
            // translate through this map before reaching real Vulkan.
            replay_context.source_to_receiver_device[*parameters.pDevice] = *parameters.pDevice;
        }
    }

    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }
};

} // namespace vkfwd::receiver::manual
