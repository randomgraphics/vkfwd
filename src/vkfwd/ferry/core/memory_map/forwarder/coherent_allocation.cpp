#include "memory_map/forwarder/coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

VkResult CoherentForwarderAllocation::map(VkDeviceSize /*offset*/, VkDeviceSize /*size*/, VkMemoryMapFlags /*flags*/, void ** ppData) {
    if (ppData) { *ppData = nullptr; }
    VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map not yet implemented (phase 0)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

void CoherentForwarderAllocation::unmap() {}

VkResult CoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult CoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
