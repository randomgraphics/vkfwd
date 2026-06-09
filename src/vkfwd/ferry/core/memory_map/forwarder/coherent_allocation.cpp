#include "memory_map/forwarder/coherent_allocation.hpp"

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

VkResult CoherentForwarderAllocation::map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) {
    // Mirror the N2 fail-closed contract: an app that ignores VkResult must
    // not be left holding a stale (or worse, receiver-process) pointer when
    // map() returns non-success.
    if (ppData) { *ppData = nullptr; }

    const VkDeviceSize allocation_size = info().allocation_size;
    if (offset > allocation_size) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map offset {} exceeds allocation_size {}", offset, allocation_size);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    const VkDeviceSize effective_size = (size == VK_WHOLE_SIZE) ? (allocation_size - offset) : size;
    if (effective_size > allocation_size - offset) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map offset+size out of range, offset={}, size={}, allocation_size={}", offset, effective_size,
                        allocation_size);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    // Reserve full allocation_size of VA so *ppData - offset is page-aligned
    // (and therefore satisfies minMemoryMapAlignment) regardless of how the
    // app picked offset. The reservation outlives any commit failure so
    // release() below always has the right base+size pair.
    void * const reservation = vm::reserve(static_cast<std::size_t>(allocation_size));
    if (!reservation) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map vm::reserve failed for allocation_size={}", allocation_size);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    const std::size_t commit_begin = vm::page_floor(static_cast<std::size_t>(offset));
    const std::size_t commit_end   = vm::page_ceil(static_cast<std::size_t>(offset + effective_size));
    if (commit_end > commit_begin) {
        if (!vm::commit(static_cast<std::uint8_t *>(reservation) + commit_begin, commit_end - commit_begin)) {
            VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map vm::commit failed, begin={}, end={}", commit_begin, commit_end);
            vm::release(reservation, static_cast<std::size_t>(allocation_size));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

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

    // The receiver synchronously calls real vkMapMemory and (for C2.1) trails
    // effective_size raw bytes after the MemoryMapResponse struct.
    CommandStream response_stream = forwarder.flush();

    auto response_view = response_stream.at<wire::MemoryMapResponse>(0, sizeof(wire::MemoryMapResponse));
    if (response_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map response stream too small ({} bytes)", response_stream.size());
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    wire::MemoryMapResponse response {};
    std::memcpy(&response, response_view.address(0), sizeof(response));

    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map response manager_revision mismatch ({} vs {})", response.manager_revision,
                        kMemoryMapManagerRevision);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    if (response.return_value != VK_SUCCESS) {
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return static_cast<VkResult>(response.return_value);
    }
    if (response.effective_size != effective_size) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map effective_size mismatch (receiver={}, source={})", response.effective_size, effective_size);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }

    // C2.1 receiver→source bracketed copy. The receiver appended exactly
    // effective_size bytes immediately after the response struct; copy them
    // into source staging at [offset, offset+effective_size). Without this
    // step a CPU read of mapped coherent memory would see uninitialized
    // staging instead of the receiver's current contents.
    if (response.initial_payload_present != 0) {
        const std::size_t payload_offset = sizeof(wire::MemoryMapResponse);
        const std::size_t payload_size   = static_cast<std::size_t>(effective_size);
        auto              payload_view   = response_stream.at(payload_offset, payload_size);
        if (payload_view.empty() && payload_size != 0) {
            VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map initial payload not addressable (need {} bytes, stream={})", payload_size,
                            response_stream.size());
            vm::release(reservation, static_cast<std::size_t>(allocation_size));
            return VK_ERROR_UNKNOWN;
        }
        std::memcpy(static_cast<std::uint8_t *>(reservation) + offset, payload_view.address(0), payload_size);
    }

    reservation_base_ = reservation;
    mapped_offset_    = offset;
    mapped_size_      = effective_size;
    if (ppData) { *ppData = static_cast<std::uint8_t *>(reservation_base_) + offset; }
    return VK_SUCCESS;
}

void CoherentForwarderAllocation::unmap() {
    // Same fail-closed guard as N2: a never-mapped or already-unmapped
    // allocation is a no-op so the destructor and a subsequent map() see
    // a clean slate either way.
    if (reservation_base_ == nullptr) { return; }

    // C2.1 unmap chunk layout:
    //   [ CommandChunkHeader ]
    //   [ MemoryUnmapRequestHeader (range_count=1) ]
    //   [ MemoryTransferRange[1] ]
    //   [ mapped_size_ raw payload bytes ]
    // payload_offset is chunk-relative (same convention as MemoryFlush).
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryUnmapRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryUnmapRequestHeader);
    constexpr std::size_t kBytesOffset      = kRangeOffset + sizeof(wire::MemoryTransferRange);
    const std::size_t     payload_bytes     = static_cast<std::size_t>(mapped_size_);
    const std::size_t     chunk_size        = kBytesOffset + payload_bytes;

    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    try {
        std::size_t chunk_offset = 0;
        auto        destination  = stream.grow<std::uint8_t>(chunk_size, ::vkfwd::CommandStream::kBaseAlignment, &chunk_offset);

        ::vkfwd::CommandChunkHeader chunk_header {};
        chunk_header.command_id       = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryUnmap);
        chunk_header.size             = static_cast<std::uint32_t>(chunk_size);
        chunk_header.command_revision = kMemoryMapManagerRevision;

        const wire::MemoryUnmapRequestHeader unmap_header {
            .manager_revision = kMemoryMapManagerRevision,
            .range_count      = 1,
            .device           = info().device,
            .memory           = info().memory,
            .mapped_offset    = mapped_offset_,
            .mapped_size      = mapped_size_,
            .reserved         = 0,
            .pad0             = 0,
        };
        const wire::MemoryTransferRange range {
            .offset         = mapped_offset_,
            .size           = mapped_size_,
            .payload_offset = kBytesOffset,
        };

        if (destination.set(0, sizeof(chunk_header), reinterpret_cast<const std::uint8_t *>(&chunk_header)) != sizeof(chunk_header) ||
            destination.set(kPayloadOffset, sizeof(unmap_header), reinterpret_cast<const std::uint8_t *>(&unmap_header)) != sizeof(unmap_header) ||
            destination.set(kRangeOffset, sizeof(range), reinterpret_cast<const std::uint8_t *>(&range)) != sizeof(range) ||
            destination.set(kBytesOffset, payload_bytes, static_cast<const std::uint8_t *>(reservation_base_) + mapped_offset_) != payload_bytes) {
            // Stream copy failed mid-grow. Release the reservation anyway so a
            // later map() does not double-reserve VA. The receiver mapping is
            // leaked (we never told it to unmap) but the local state is now
            // consistent — the divergence is logged for triage.
            VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::unmap could not copy chunk bytes (chunk_size={}); releasing local reservation anyway",
                            chunk_size);
            vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));
            reservation_base_ = nullptr;
            mapped_offset_    = 0;
            mapped_size_      = 0;
            return;
        }
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::unmap out of host memory (chunk_size={}); releasing local reservation anyway", chunk_size);
        vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));
        reservation_base_ = nullptr;
        mapped_offset_    = 0;
        mapped_size_      = 0;
        return;
    }

    // Synchronous flush so the receiver finishes its unmap (and consumes the
    // payload bytes) before we release the source-side VA the payload was
    // sourced from. In a non-loopback transport the receiver lives in another
    // process; the same ordering invariant still applies because the bytes
    // travel through the transport's own buffering.
    (void) forwarder.flush();

    vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));

    reservation_base_ = nullptr;
    mapped_offset_    = 0;
    mapped_size_      = 0;
}

// Coherent host-visible memory's Vulkan contract makes vkFlushMappedMemoryRanges
// and vkInvalidateMappedMemoryRanges no-ops — the spec guarantees the host and
// device see each other's writes without an explicit barrier. A
// spec-compliant app should therefore NEVER call flush or invalidate on a
// coherent allocation. Returning VK_ERROR_FEATURE_NOT_PRESENT is intentional:
// it is the loudest signal we can give a buggy app that the call shouldn't
// have happened. C2.1 covers the common one-shot patterns (map → write →
// unmap and map → read → unmap) via the bracketed copies in map()/unmap();
// the persistent-coherent-map case (write during the mapped lifetime, GPU
// reads via coherent semantics, then more host writes without unmap) needs
// C2.3's sync-point copies, which is future Phase 3c work.
VkResult CoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::flush called on coherent memory (spec says flush is a no-op for coherent; C2.3 not yet implemented)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult CoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    VKFWD_LOG_ERROR(
        "vkfwd: CoherentForwarderAllocation::invalidate called on coherent memory (spec says invalidate is a no-op for coherent; C2.3 not yet implemented)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
