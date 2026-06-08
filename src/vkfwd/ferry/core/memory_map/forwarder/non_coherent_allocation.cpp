#include "memory_map/forwarder/non_coherent_allocation.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "forwarder.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/manual_chunk.hpp"
#include "memory_map/vm_primitives.hpp"
#include "memory_map/wire_format.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vkfwd::memory_map {

VkResult NonCoherentForwarderAllocation::map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) {
    // Defensive: a caller that ignores VkResult must not be left holding a
    // stale receiver-side mapped pointer if we fail before *ppData is written.
    if (ppData) { *ppData = nullptr; }

    const VkDeviceSize allocation_size = info().allocation_size;

    // VK_WHOLE_SIZE resolves against the allocation extent. Bounds errors here
    // would also be caught by the receiver, but the local-first protocol
    // demands we reject obviously-invalid arguments before reserving VA.
    if (offset > allocation_size) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map offset {} exceeds allocation_size {}", offset, allocation_size);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    const VkDeviceSize effective_size = (size == VK_WHOLE_SIZE) ? (allocation_size - offset) : size;
    if (effective_size > allocation_size - offset) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map offset+size out of range, offset={}, size={}, allocation_size={}", offset, effective_size,
                        allocation_size);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    // Step 2: reserve the full allocation_size of VA so *ppData - offset is
    // host-page-aligned (and therefore satisfies minMemoryMapAlignment, which
    // the allocate-time check upper-bounded by host_page_size()).
    void * const reservation = vm::reserve(static_cast<std::size_t>(allocation_size));
    if (!reservation) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map vm::reserve failed for allocation_size={}", allocation_size);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Step 3: commit only the page-aligned span covering [offset, offset+effective_size).
    // The reservation stays alive even if commit fails so release() below has
    // the matching base+size pair the OS expects.
    const std::size_t commit_begin = vm::page_floor(static_cast<std::size_t>(offset));
    const std::size_t commit_end   = vm::page_ceil(static_cast<std::size_t>(offset + effective_size));
    if (commit_end > commit_begin) {
        if (!vm::commit(static_cast<std::uint8_t *>(reservation) + commit_begin, commit_end - commit_begin)) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map vm::commit failed, begin={}, end={}", commit_begin, commit_end);
            vm::release(reservation, static_cast<std::size_t>(allocation_size));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    // Step 4: build the manual MemoryMap chunk carrying source-resolved
    // classification so the receiver can lazily build its allocation without
    // its own MemoryTypeRegistry.
    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    const wire::MemoryMapRequest request {
        .manager_revision         = kMemoryMapManagerRevision,
        .memory_type_index        = info().memory_type_index,
        .device                   = info().device,
        .memory                   = info().memory,
        .offset                   = offset,
        .size                     = effective_size,
        .flags                    = flags,
        .pad0                     = 0,
        .property_flags           = info().property_flags,
        .pad1                     = 0,
        .allocation_size          = allocation_size,
        .non_coherent_atom_size   = info().non_coherent_atom_size,
        .min_memory_map_alignment = info().min_memory_map_alignment,
    };
    const VkResult append_result = append_manual_chunk(stream, ::vkfwd::manual::CommandId::MemoryMap, request);
    if (append_result != VK_SUCCESS) {
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return append_result;
    }

    // Step 5: synchronous flush; the receiver responds with a MemoryMapResponse.
    CommandStream response_stream = forwarder.flush();

    // Step 6 + 7: read and validate the response. Every failure releases the
    // reservation before returning so a rejected map leaves no VA leaked.
    auto response_view = response_stream.at<wire::MemoryMapResponse>(0, sizeof(wire::MemoryMapResponse));
    if (response_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map response stream too small ({} bytes)", response_stream.size());
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    wire::MemoryMapResponse response {};
    std::memcpy(&response, response_view.address(0), sizeof(response));

    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map response manager_revision mismatch ({} vs {})", response.manager_revision,
                        kMemoryMapManagerRevision);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    if (response.return_value != VK_SUCCESS) {
        // Receiver-rejected map: propagate its VkResult so the caller sees the
        // true driver-side cause (OOM, invalid handle, etc.).
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return static_cast<VkResult>(response.return_value);
    }
    if (response.effective_size != effective_size) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map effective_size mismatch (receiver={}, source={})", response.effective_size, effective_size);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }

    // Step 9: success — record the reservation so unmap()/destructor can
    // release it, and write the page-aligned-base + offset pointer to ppData.
    reservation_base_ = reservation;
    mapped_offset_    = offset;
    mapped_size_      = effective_size;
    if (ppData) { *ppData = static_cast<std::uint8_t *>(reservation_base_) + offset; }
    return VK_SUCCESS;
}

void NonCoherentForwarderAllocation::unmap() {
    // Step 1: guard against never-mapped or already-unmapped allocations.
    // map() leaves reservation_base_ == nullptr on every failure path, so this
    // check also covers the "map rejected, app called unmap anyway" case.
    if (reservation_base_ == nullptr) { return; }

    // Step 2: emit a header-only manual MemoryUnmap chunk. range_count == 0
    // under N2 — any host writes the app cared about should have been
    // delivered by an earlier vkFlushMappedMemoryRanges (Phase 2). Carrying
    // mapped_offset/mapped_size lets the receiver release exactly the span it
    // mapped without consulting its own per-handle bookkeeping.
    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    const wire::MemoryUnmapRequestHeader request {
        .manager_revision = kMemoryMapManagerRevision,
        .range_count      = 0,
        .device           = info().device,
        .memory           = info().memory,
        .mapped_offset    = mapped_offset_,
        .mapped_size      = mapped_size_,
        .reserved         = 0,
        .pad0             = 0,
    };
    const VkResult append_result = append_manual_chunk(stream, ::vkfwd::manual::CommandId::MemoryUnmap, request);
    if (append_result != VK_SUCCESS) {
        // The chunk could not be queued. We still must release the local VA so
        // a later map() does not double-reserve; log the divergence so the
        // receiver-side leak (its mapping is still live) is visible.
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::unmap failed to append chunk; releasing local reservation anyway");
        vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));
        reservation_base_ = nullptr;
        mapped_offset_    = 0;
        mapped_size_      = 0;
        return;
    }

    // Step 3: synchronous flush. The N2 unmap response is empty by design;
    // its only purpose is to gate the local release on the receiver having
    // actually finished its unmap, so no reader of the staging pages remains
    // when we release the reservation below.
    (void) forwarder.flush();

    // Step 4: release the reservation only AFTER flush returns. Doing it
    // before would let the OS reuse the VA while the receiver is still
    // touching it (in non-loopback transports the receiver lives in another
    // process, but the same invariant holds: release ordering is the
    // forwarder's responsibility).
    vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));

    // Step 5: clear all mapping state so the destructor does not double-free
    // and a subsequent map() observes a clean slate.
    reservation_base_ = nullptr;
    mapped_offset_    = 0;
    mapped_size_      = 0;
}

VkResult NonCoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    // Phase 2 work; non-coherent flush/invalidate require the
    // vkFlushMappedMemoryRanges/vkInvalidateMappedMemoryRanges wire chunks
    // that aren't generated yet.
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult NonCoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    // See flush(): wired up in Phase 2.
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
