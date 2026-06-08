#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 1 fills these methods in with real N2 behavior
// (source-owned staging, no synced-range tracking — see
// doc/memory_map_management.md for the rejected-N3 rationale). Until then
// every method returns VK_ERROR_FEATURE_NOT_PRESENT so callers cannot
// mistake the placeholder for working mapped-memory support.
class NonCoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;
};

} // namespace vkfwd::memory_map
