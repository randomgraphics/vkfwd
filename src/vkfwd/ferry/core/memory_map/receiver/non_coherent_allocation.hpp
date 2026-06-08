#pragma once

#include "memory_map/receiver_allocation.hpp"

#include <cstddef>

namespace vkfwd::memory_map {

// Phase 1 N2 strategy on the receiver side: map_endpoint() forwards the
// source map request to the local Vulkan driver and remembers the resulting
// receiver-native pointer for later flush/unmap. The pointer is intentionally
// private — it is a receiver-process address that must never leak into the
// source-visible response payload (the forwarder reconstructs its own VA via
// vm::reserve+commit; the wire only carries effective_size and VkResult).
class NonCoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;

private:
    // Receiver-native mapped pointer the driver wrote in vkMapMemory. Held so
    // Task 10's unmap_endpoint and Phase 2's flush/invalidate endpoints can
    // address the right base without re-mapping. Never exposed via an
    // accessor: the address only makes sense inside the receiver process.
    void *       receiver_mapped_ptr_ = nullptr;
    VkDeviceSize mapped_offset_       = 0;
    VkDeviceSize mapped_size_         = 0;
};

} // namespace vkfwd::memory_map
