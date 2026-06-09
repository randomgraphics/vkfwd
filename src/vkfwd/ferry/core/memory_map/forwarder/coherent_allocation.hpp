#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase 3a C2.1 strategy: same source-owned staging as the non-coherent N2
// path (vm::reserve + commit) PLUS a bracketed byte transfer on map and
// unmap. The map response carries effective_size bytes the forwarder copies
// into source staging so a CPU read after vkMapMemory sees the receiver's
// current contents (the use case is the host reading GPU compute output
// without an explicit invalidate, which the spec permits for coherent
// memory). unmap pushes the source staging bytes back to the receiver so a
// CPU write before vkUnmapMemory reaches device memory without an explicit
// flush. flush() / invalidate() are spec-no-ops for coherent memory and
// remain VK_ERROR_FEATURE_NOT_PRESENT so any app that incorrectly calls them
// produces a loud error; persistent coherent maps (write-during-mapped or
// read-during-mapped without remapping) need C2.3's sync-point copies — that
// is future Phase 3c work.
class CoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;

private:
    // Mirrors the N2 forwarder's bookkeeping. reservation_base_ != nullptr
    // iff the allocation is currently mapped; unmap() clears all three and
    // every map() failure path releases the reservation without storing
    // anything here so the destructor never double-frees.
    void *       reservation_base_ = nullptr;
    VkDeviceSize mapped_offset_    = 0;
    VkDeviceSize mapped_size_      = 0;
};

} // namespace vkfwd::memory_map
