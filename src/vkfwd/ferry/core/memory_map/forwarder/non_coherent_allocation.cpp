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

namespace {

// nonCoherentAtomSize rounding: the receiver-side real vkFlushMappedMemoryRanges
// requires VkMappedMemoryRange.offset to be a multiple of nonCoherentAtomSize
// and either size to also be a multiple OR (offset + size) to equal the
// allocation extent (VkMemoryAllocateInfo::allocationSize) — the second case
// is what lets flush(0, VK_WHOLE_SIZE) work even when allocation_size is not
// atom-aligned. We expand inwards-out (round offset down, end up) here; the
// receiver clamps the result to the mapped extent.
struct AtomAlignedRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size   = 0;
};
AtomAlignedRange atom_align(VkDeviceSize offset, VkDeviceSize size, VkDeviceSize atom_size, VkDeviceSize mapped_offset, VkDeviceSize mapped_size) {
    if (atom_size == 0) { return {offset, size}; }
    const VkDeviceSize aligned_offset = (offset / atom_size) * atom_size;
    VkDeviceSize       aligned_end    = offset + size;
    const VkDeviceSize remainder      = aligned_end % atom_size;
    if (remainder != 0) { aligned_end += atom_size - remainder; }
    // Clamp the expansion to the mapped span — expanding past it would push
    // the receiver-side range outside the mapped pointer's addressable region.
    const VkDeviceSize mapped_end = mapped_offset + mapped_size;
    if (aligned_end > mapped_end) { aligned_end = mapped_end; }
    return {aligned_offset, aligned_end - aligned_offset};
}

} // namespace

VkResult NonCoherentForwarderAllocation::flush(VkDeviceSize offset, VkDeviceSize size) {
    // Step 1: validate active mapping. Real apps would only call flush on a
    // mapped allocation; we still need to reject an out-of-contract call
    // before reading reservation_base_.
    if (reservation_base_ == nullptr) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush called on unmapped allocation");
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    // Step 2: bound the requested range against the actively mapped extent.
    // Anything outside [mapped_offset_, mapped_offset_ + mapped_size_) cannot
    // be backed by committed source staging.
    const VkDeviceSize mapped_end    = mapped_offset_ + mapped_size_;
    const VkDeviceSize requested_end = offset + size;
    if (offset < mapped_offset_ || requested_end > mapped_end || requested_end < offset) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush range [{},{}) outside mapped [{},{})", offset, requested_end, mapped_offset_, mapped_end);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    // Step 3: atom-align offset down and end up. The receiver applies the
    // exact same alignment when constructing its VkMappedMemoryRange so a
    // mismatch would be a same-build wire-format bug.
    const auto aligned = atom_align(offset, size, info().non_coherent_atom_size, mapped_offset_, mapped_size_);
    if (aligned.size == 0) { return VK_SUCCESS; }

    // Step 4: build a single MemoryFlush chunk carrying:
    //   CommandChunkHeader + MemoryFlushRequestHeader + MemoryTransferRange[1] + payload bytes.
    // All in one grow() so payload_offset stays chunk-relative under a
    // straightforward layout. Alignment of the largest payload element
    // (MemoryFlushRequestHeader / MemoryTransferRange — both 8-byte aligned)
    // dictates the offset of the request header; the raw bytes have no
    // alignment requirement so they tail-pack against the range array.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryFlushRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryFlushRequestHeader);
    constexpr std::size_t kBytesOffset      = kRangeOffset + sizeof(wire::MemoryTransferRange);
    const std::size_t     payload_bytes     = static_cast<std::size_t>(aligned.size);
    const std::size_t     chunk_size        = kBytesOffset + payload_bytes;

    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    std::size_t chunk_offset = 0;
    try {
        auto destination = stream.grow<std::uint8_t>(chunk_size, ::vkfwd::CommandStream::kBaseAlignment, &chunk_offset);

        ::vkfwd::CommandChunkHeader chunk_header {};
        chunk_header.command_id       = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryFlush);
        chunk_header.size             = static_cast<std::uint32_t>(chunk_size);
        chunk_header.command_revision = kMemoryMapManagerRevision;

        const wire::MemoryFlushRequestHeader flush_header {
            .manager_revision = kMemoryMapManagerRevision,
            .range_count      = 1,
            .device           = info().device,
            .memory           = info().memory,
        };
        const wire::MemoryTransferRange range {
            .offset         = aligned.offset,
            .size           = aligned.size,
            .payload_offset = kBytesOffset,
        };

        if (destination.set(0, sizeof(chunk_header), reinterpret_cast<const std::uint8_t *>(&chunk_header)) != sizeof(chunk_header) ||
            destination.set(kPayloadOffset, sizeof(flush_header), reinterpret_cast<const std::uint8_t *>(&flush_header)) != sizeof(flush_header) ||
            destination.set(kRangeOffset, sizeof(range), reinterpret_cast<const std::uint8_t *>(&range)) != sizeof(range) ||
            destination.set(kBytesOffset, payload_bytes, static_cast<const std::uint8_t *>(reservation_base_) + aligned.offset) != payload_bytes) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush could not copy chunk bytes (chunk_size={})", chunk_size);
            return VK_ERROR_UNKNOWN;
        }
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush out of host memory (chunk_size={})", chunk_size);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Step 5: synchronous round-trip. The receiver writes bytes into its
    // mapped pointer, calls real vkFlushMappedMemoryRanges, and returns the
    // driver's VkResult on the response stream.
    CommandStream response_stream = forwarder.flush();

    auto response_view = response_stream.at<wire::MemoryFlushResponse>(0, sizeof(wire::MemoryFlushResponse));
    if (response_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush response stream too small ({} bytes)", response_stream.size());
        return VK_ERROR_UNKNOWN;
    }
    wire::MemoryFlushResponse response {};
    std::memcpy(&response, response_view.address(0), sizeof(response));
    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::flush manager_revision mismatch ({} vs {})", response.manager_revision,
                        kMemoryMapManagerRevision);
        return VK_ERROR_UNKNOWN;
    }
    return static_cast<VkResult>(response.return_value);
}

VkResult NonCoherentForwarderAllocation::invalidate(VkDeviceSize offset, VkDeviceSize size) {
    // Same validation as flush; the bytes flow the other direction so there
    // is no outbound payload but we still need the mapped-range bounds for
    // local correctness.
    if (reservation_base_ == nullptr) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate called on unmapped allocation");
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    const VkDeviceSize mapped_end    = mapped_offset_ + mapped_size_;
    const VkDeviceSize requested_end = offset + size;
    if (offset < mapped_offset_ || requested_end > mapped_end || requested_end < offset) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate range [{},{}) outside mapped [{},{})", offset, requested_end, mapped_offset_,
                        mapped_end);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    const auto aligned = atom_align(offset, size, info().non_coherent_atom_size, mapped_offset_, mapped_size_);
    if (aligned.size == 0) { return VK_SUCCESS; }

    // Invalidate request is purely metadata — no outbound payload. The
    // response carries the receiver bytes back.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryInvalidateRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryInvalidateRequestHeader);
    constexpr std::size_t kChunkSize        = kRangeOffset + sizeof(wire::MemoryInvalidateRangeRequest);

    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    std::size_t chunk_offset = 0;
    try {
        auto destination = stream.grow<std::uint8_t>(kChunkSize, ::vkfwd::CommandStream::kBaseAlignment, &chunk_offset);

        ::vkfwd::CommandChunkHeader chunk_header {};
        chunk_header.command_id       = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryInvalidate);
        chunk_header.size             = static_cast<std::uint32_t>(kChunkSize);
        chunk_header.command_revision = kMemoryMapManagerRevision;

        const wire::MemoryInvalidateRequestHeader inv_header {
            .manager_revision = kMemoryMapManagerRevision,
            .range_count      = 1,
            .device           = info().device,
            .memory           = info().memory,
        };
        const wire::MemoryInvalidateRangeRequest range {
            .offset = aligned.offset,
            .size   = aligned.size,
        };

        if (destination.set(0, sizeof(chunk_header), reinterpret_cast<const std::uint8_t *>(&chunk_header)) != sizeof(chunk_header) ||
            destination.set(kPayloadOffset, sizeof(inv_header), reinterpret_cast<const std::uint8_t *>(&inv_header)) != sizeof(inv_header) ||
            destination.set(kRangeOffset, sizeof(range), reinterpret_cast<const std::uint8_t *>(&range)) != sizeof(range)) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate could not copy chunk bytes (chunk_size={})", kChunkSize);
            return VK_ERROR_UNKNOWN;
        }
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate out of host memory (chunk_size={})", kChunkSize);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    CommandStream response_stream = forwarder.flush();

    auto response_view = response_stream.at<wire::MemoryInvalidateResponseHeader>(0, sizeof(wire::MemoryInvalidateResponseHeader));
    if (response_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate response stream too small ({} bytes)", response_stream.size());
        return VK_ERROR_UNKNOWN;
    }
    wire::MemoryInvalidateResponseHeader response {};
    std::memcpy(&response, response_view.address(0), sizeof(response));
    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate manager_revision mismatch ({} vs {})", response.manager_revision,
                        kMemoryMapManagerRevision);
        return VK_ERROR_UNKNOWN;
    }
    const VkResult driver_result = static_cast<VkResult>(response.return_value);

    // The response_stream layout is:
    //   [ MemoryInvalidateResponseHeader ]
    //   [ MemoryTransferRange[range_count] ]
    //   [ raw bytes ]
    // The header lands at offset 0 because the receiver's first grow() on an
    // empty stream lands at offset 0 (kBaseAlignment is a multiple of the
    // header's alignof). The range array follows immediately because the
    // header is sized to a multiple of MemoryTransferRange's alignment.
    constexpr std::size_t kHeaderSize = sizeof(wire::MemoryInvalidateResponseHeader);
    static_assert(kHeaderSize % alignof(wire::MemoryTransferRange) == 0,
                  "MemoryInvalidateResponseHeader must be a multiple of MemoryTransferRange alignment so the trailing range[] is naturally aligned");

    // Copy receiver bytes back into source staging for each range. Even if
    // the receiver-side driver returned a non-success VkResult, the response
    // might still carry zero ranges; treat that uniformly.
    for (std::uint32_t i = 0; i < response.range_count; ++i) {
        const std::size_t range_offset_in_stream = kHeaderSize + static_cast<std::size_t>(i) * sizeof(wire::MemoryTransferRange);
        auto              range_view             = response_stream.at<wire::MemoryTransferRange>(range_offset_in_stream, sizeof(wire::MemoryTransferRange));
        if (range_view.empty()) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate response range[{}] not addressable", i);
            return VK_ERROR_UNKNOWN;
        }
        wire::MemoryTransferRange resp_range {};
        std::memcpy(&resp_range, range_view.address(0), sizeof(resp_range));

        // Defensive bounds: the receiver echoes the source-supplied range, so
        // it must lie inside the actively mapped extent. A divergence is a
        // wire-format bug — copying anyway could splatter unrelated source VA.
        if (resp_range.offset < mapped_offset_ || resp_range.offset + resp_range.size > mapped_offset_ + mapped_size_ ||
            resp_range.offset + resp_range.size < resp_range.offset) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate response range[{}] [{},{}) outside mapped [{},{})", i, resp_range.offset,
                            resp_range.offset + resp_range.size, mapped_offset_, mapped_offset_ + mapped_size_);
            return VK_ERROR_UNKNOWN;
        }

        auto bytes_view = response_stream.at(static_cast<std::size_t>(resp_range.payload_offset), static_cast<std::size_t>(resp_range.size));
        if (bytes_view.empty() && resp_range.size != 0) {
            VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::invalidate response range[{}] payload not addressable (offset={}, size={})", i,
                            resp_range.payload_offset, resp_range.size);
            return VK_ERROR_UNKNOWN;
        }
        std::memcpy(static_cast<std::uint8_t *>(reservation_base_) + resp_range.offset, bytes_view.address(0), static_cast<std::size_t>(resp_range.size));
    }

    return driver_result;
}

} // namespace vkfwd::memory_map
