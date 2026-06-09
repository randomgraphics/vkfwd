#pragma once

#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {

// Receiver-side C2.1 partner of CoherentForwarderAllocation. map_endpoint
// drives the local driver's vkMapMemory and appends the resulting mapped
// bytes after the MemoryMapResponse so the forwarder can copy them into
// source staging. unmap_endpoint reads the source-supplied payload bytes,
// writes them into the still-live mapped pointer, then calls vkUnmapMemory.
// flush/invalidate endpoints stay rejection stubs (return false) because
// the wire never sends those chunks for a coherent allocation — the
// forwarder rejects with VK_ERROR_FEATURE_NOT_PRESENT before they hit the
// transport.
class CoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;

private:
    // Receiver-native mapped pointer the driver wrote in vkMapMemory. Held
    // until unmap_endpoint so the source-supplied bracketed bytes have a
    // destination to land in. Never exposed via an accessor: the address
    // only makes sense inside the receiver process and must never appear on
    // the wire (the forwarder reconstructs its own VA via vm::reserve+commit).
    void *       receiver_mapped_ptr_ = nullptr;
    VkDeviceSize mapped_offset_       = 0;
    VkDeviceSize mapped_size_         = 0;
};

} // namespace vkfwd::memory_map
