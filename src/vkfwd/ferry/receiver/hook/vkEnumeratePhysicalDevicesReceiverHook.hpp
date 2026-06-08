#pragma once

#include "generated/command/vkEnumeratePhysicalDevices.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::EnumeratePhysicalDevices> {
    // The endpoint stub reads every *_enabled flag explicitly, so a specialization
    // must declare them all — even the ones that just inherit the default false.
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
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }

    // Records source-visible -> receiver-native VkPhysicalDevice mappings so
    // the QueryPhysicalDeviceMemoryInfo manual command (used by the forwarder
    // when the MemoryTypeRegistry misses) can translate the handle before
    // calling the real vkGetPhysicalDevice{Memory,}Properties PFNs. Phase 1's
    // single-process loopback gives (h, h) pairs; a remote receiver that mints
    // distinct handles populates the same map without any code change.
    //
    // vkEnumeratePhysicalDevices is a two-call pattern: first call returns
    // only the count, second call returns the handles. The hook runs on every
    // call; the count-only invocation has pPhysicalDevices == nullptr and is a
    // no-op here.
    static void after_call(const ::vkfwd::generated::commands::vkEnumeratePhysicalDevices::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkEnumeratePhysicalDevices::Command::Response &   response,
                           ::vkfwd::receiver::ReplayContext &                                                    replay_context) {
        // VK_INCOMPLETE returns valid (but truncated) handles too; only an
        // outright failure should skip recording.
        if (response.return_value != VK_SUCCESS && response.return_value != VK_INCOMPLETE) { return; }
        if (parameters.pPhysicalDevices == nullptr || parameters.pPhysicalDeviceCount == nullptr) { return; }
        const std::uint32_t count = *parameters.pPhysicalDeviceCount;
        for (std::uint32_t i = 0; i < count; ++i) {
            VkPhysicalDevice handle = parameters.pPhysicalDevices[i];
            if (handle == VK_NULL_HANDLE) { continue; }
            replay_context.source_to_receiver_physical_device[handle] = handle;
        }
    }
};

} // namespace vkfwd::receiver::manual
