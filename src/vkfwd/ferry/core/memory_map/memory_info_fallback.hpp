#pragma once

#include <vulkan/vulkan.h>

namespace vkfwd::memory_map {

// Synchronous fallback: when MemoryTypeRegistry::resolve() misses at
// vkAllocateMemory time, send a QueryPhysicalDeviceMemoryInfo manual chunk to
// the receiver, ingest the response into the registry, and return. Always a
// no-op when the device->physical-device mapping is unknown (vkCreateDevice
// has not been observed); the allocation will then remain untracked and a
// subsequent vkMapMemory on it will surface VK_ERROR_FEATURE_NOT_PRESENT.
//
// Why a helper, not inline in the hook: the hook header is included by the
// generated entry stub which already pulls in heavy generated headers; isolating
// this helper in a TU keeps Forwarder + CommandStream linkage out of the hook
// translation unit, which the generator also drags into receiver tests via
// command-headers-only includes.
void request_memory_info_fallback(VkDevice device);

} // namespace vkfwd::memory_map
