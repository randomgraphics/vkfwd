#include "memory_map/forwarder/non_coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

VkResult NonCoherentForwarderAllocation::map(VkDeviceSize /*offset*/, VkDeviceSize /*size*/, VkMemoryMapFlags /*flags*/, void ** ppData) {
    // Zero the app's output pointer so a phase-0 caller cannot accidentally
    // dereference a stale value if it ignores the error return.
    if (ppData) { *ppData = nullptr; }
    VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map not yet implemented (phase 0)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

void NonCoherentForwarderAllocation::unmap() {}

VkResult NonCoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult NonCoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
