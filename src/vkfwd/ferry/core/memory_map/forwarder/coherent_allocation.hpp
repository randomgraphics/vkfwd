#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 3 (or whichever phase ships the chosen
// coherent strategy) fills these methods in. Until then every method
// returns VK_ERROR_FEATURE_NOT_PRESENT.
class CoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;
};

} // namespace vkfwd::memory_map
