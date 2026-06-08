#pragma once

#include "generated/command/vkCreateInstance.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateInstance> {
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

    static void after_call(const ::vkfwd::generated::commands::vkCreateInstance::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkCreateInstance::Command::Response &   response,
                           ::vkfwd::receiver::ReplayContext &                                          replay_context) {
        // Successful receiver-side instance creation changes the destination
        // dispatch scope. Keep this in ReplayContext so tests and transports do
        // not patch host callbacks around the generated endpoint contract.
        if (response.return_value == VK_SUCCESS && parameters.pInstance && *parameters.pInstance) {
            replay_context.dispatch.instance.init(*parameters.pInstance, replay_context.dispatch.global.get_instance_proc_addr);
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
