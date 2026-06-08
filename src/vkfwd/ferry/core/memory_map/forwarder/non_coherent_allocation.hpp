#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase 1 N2 strategy: source-owned staging via VM reserve+commit, no
// synced-range tracking. map() reserves the full allocation_size of source
// VA up-front so *ppData - offset is page-aligned (and therefore satisfies
// VkPhysicalDeviceLimits::minMemoryMapAlignment) by construction.
// flush() / invalidate() remain VK_ERROR_FEATURE_NOT_PRESENT until Phase 2
// wires them — apps that write to mapped memory will not see writes propagate
// to the receiver under Phase 1 alone. That is intentional and not a
// corruption hazard (the receiver's mapped pointer is never exposed to the
// app), but it makes flush/invalidate the next required step.
class NonCoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;

private:
    // Active mapping state. reservation_base_ != nullptr iff the allocation is
    // currently mapped. unmap() (Task 8) clears all three; map() failure paths
    // release the reservation without storing anything here so the destructor
    // does not double-free.
    void *       reservation_base_ = nullptr;
    VkDeviceSize mapped_offset_    = 0;
    VkDeviceSize mapped_size_      = 0;
};

} // namespace vkfwd::memory_map
