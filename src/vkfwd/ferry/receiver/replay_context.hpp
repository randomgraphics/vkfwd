#pragma once

#include "generated/dispatch_table.hpp"
#include "memory_map/manager.hpp"

#include <vulkan/vulkan.h>

#include <unordered_map>

namespace vkfwd::receiver {

struct ReplayContext {
    // Receiver replay needs a mutable context instead of a bare const dispatch
    // table because successful create/destroy commands change what can be
    // replayed next.
    ::vkfwd::generated::DistributionTable dispatch {};

    // Per-context memory map manager: owns receiver-side mapped ranges and the
    // staging-byte transfer protocol for vkMapMemory / vkUnmapMemory.
    ::vkfwd::MemoryMapReceiver memoryMap;

    // Source-visible -> receiver-native handle translation. Source handles
    // arrive in wire payloads (including the manual memory-map command chunks);
    // the receiver MUST translate before calling any real Vulkan function.
    // Populated by after-call hooks on vkCreateDevice / vkAllocateMemory.
    // vkFreeMemory clears its entry via a before-call hook. vkDestroyDevice
    // cleanup is intentionally not wired in Phase 1: VkDevice handles outlive
    // every allocation that references them, and the entire ReplayContext is
    // destroyed when the source-thread stream closes, so leaking a device
    // entry until that point is harmless. Add a vkDestroyDeviceReceiverHook
    // here if cross-context handle reuse ever becomes a concern.
    //
    // Phase 1 invariant: source and receiver are in the same process via the
    // loopback runtime, and the receiver writes its handle back into the
    // response which the forwarder propagates to the app — so the map ends up
    // recording (h, h) pairs. The map is still load-bearing because future
    // remote receivers that mint distinct handles will populate it the same
    // way.
    std::unordered_map<VkDevice, VkDevice>             source_to_receiver_device;
    std::unordered_map<VkDeviceMemory, VkDeviceMemory> source_to_receiver_memory;

    // Populated by the vkEnumeratePhysicalDevices after_call hook. Required by
    // the QueryPhysicalDeviceMemoryInfo manual dispatch path so the receiver
    // can translate a source VkPhysicalDevice to its receiver-native form
    // before calling the real vkGetPhysicalDevice{Memory,}Properties PFNs.
    // Same Phase 1 (h, h) invariant as the other two maps.
    std::unordered_map<VkPhysicalDevice, VkPhysicalDevice> source_to_receiver_physical_device;
};

} // namespace vkfwd::receiver
