#include "memory_map/receiver/non_coherent_allocation.hpp"

#include "command_stream.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/wire_format.hpp"
#include "replay_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vkfwd::memory_map {

namespace {

// Append a MemoryMapResponse to response_stream at the very front of the
// stream's logical bytes. The forwarder reads the response via
// `response_stream.at<MemoryMapResponse>(0, ...)`, so the first grow on an
// empty stream must produce offset 0. CommandStream::kBaseAlignment (128) is a
// multiple of alignof(MemoryMapResponse), so requesting the smaller alignment
// here still lands at offset 0 on a fresh stream.
void pack_response(::vkfwd::CommandStream & response_stream, const wire::MemoryMapResponse & response) {
    auto destination = response_stream.grow<std::uint8_t>(sizeof(response), alignof(wire::MemoryMapResponse));
    std::memcpy(destination.address(0), &response, sizeof(response));
}

} // namespace

bool NonCoherentReceiverAllocation::map_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range & request_range,
                                                 ::vkfwd::CommandStream & response_stream, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryMapRequest out of the manual command chunk. Payload
    // sits at the CommandChunkHeader-aligned offset within the chunk; mirror
    // the forwarder-side append math exactly so a future header-layout change
    // breaks both sides at the same revision boundary.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryMapRequest);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    if (request_range.size < kPayloadOffset + sizeof(wire::MemoryMapRequest)) {
        // Genuine protocol error (chunk too short to contain its declared
        // payload). Return false so ReceiverSession can abort the stream — a
        // packed VkResult would be a lie since we cannot even read the
        // request's manager_revision.
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::map_endpoint chunk too small ({} bytes, need {})", request_range.size,
                        kPayloadOffset + sizeof(wire::MemoryMapRequest));
        return false;
    }
    wire::MemoryMapRequest req {};
    const auto             view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(req));
    if (view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::map_endpoint request payload not addressable in stream");
        return false;
    }
    std::memcpy(&req, view.address(0), sizeof(req));

    // Step 2: per-call protocol-error handling — every failure from here on
    // packs a MemoryMapResponse and returns true so the session keeps running.
    // The forwarder turns a non-success return_value into the corresponding
    // VkResult for the application.
    if (req.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::map_endpoint request manager_revision mismatch ({} vs {})", req.manager_revision,
                        kMemoryMapManagerRevision);
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision = kMemoryMapManagerRevision,
                                           .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size   = 0,
                                       });
        return true;
    }

    // Step 3: translate source-visible handles to receiver-native ones. The
    // source process never sees the receiver's real Vulkan handles, so map()
    // arrives carrying source values that mean nothing to the local driver.
    const auto device_entry = replay_context.source_to_receiver_device.find(req.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(req.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::map_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(req.device), static_cast<const void *>(req.memory));
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision = kMemoryMapManagerRevision,
                                           .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size   = 0,
                                       });
        return true;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    // Step 4: resolve VK_WHOLE_SIZE against the recorded allocation_size. The
    // forwarder has already resolved it before sending, but the wire still
    // permits the sentinel through; re-resolving here keeps the receiver
    // robust to a future forwarder that defers the substitution.
    const VkDeviceSize allocation_size = info().allocation_size;
    const VkDeviceSize effective_size  = (req.size == VK_WHOLE_SIZE) ? (allocation_size - req.offset) : req.size;

    // Step 5: drive the local driver. The dispatch table is owned by the
    // ReplayContext (per source-thread stream) so no locking is needed.
    auto * const map_memory = replay_context.dispatch.device.map_memory;
    if (map_memory == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::map_endpoint dispatch table missing vkMapMemory");
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision = kMemoryMapManagerRevision,
                                           .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size   = 0,
                                       });
        return true;
    }
    void *         receiver_ptr  = nullptr;
    const VkResult driver_result = map_memory(receiver_device, receiver_memory, req.offset, effective_size, req.flags, &receiver_ptr);
    if (driver_result != VK_SUCCESS) {
        // Surface the driver's exact VkResult so the source-side app sees the
        // real cause (OOM, invalid handle, etc.) rather than VK_ERROR_UNKNOWN.
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision = kMemoryMapManagerRevision,
                                           .return_value     = static_cast<std::int32_t>(driver_result),
                                           .effective_size   = 0,
                                       });
        return true;
    }

    // Step 6: success — record the mapping for Task 10's unmap and Phase 2's
    // flush/invalidate. receiver_mapped_ptr_ never leaves the receiver.
    receiver_mapped_ptr_ = receiver_ptr;
    mapped_offset_       = req.offset;
    mapped_size_         = effective_size;

    pack_response(response_stream, wire::MemoryMapResponse {
                                       .manager_revision = kMemoryMapManagerRevision,
                                       .return_value     = static_cast<std::int32_t>(VK_SUCCESS),
                                       .effective_size   = effective_size,
                                   });
    return true;
}

bool NonCoherentReceiverAllocation::unmap_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range &            request_range,
                                                   ::vkfwd::CommandStream & /*response_stream*/, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryUnmapRequestHeader out of the manual command chunk.
    // Mirror the forwarder-side append_manual_chunk math so any header-layout
    // change breaks both sides at the same revision boundary.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryUnmapRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    if (request_range.size < kPayloadOffset + sizeof(wire::MemoryUnmapRequestHeader)) {
        // Chunk too short to contain its declared payload — session-fatal
        // protocol error. Unmap has no response payload, so we can only signal
        // failure by returning false; ReceiverSession will abort the stream.
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint chunk too small ({} bytes, need {})", request_range.size,
                        kPayloadOffset + sizeof(wire::MemoryUnmapRequestHeader));
        return false;
    }
    wire::MemoryUnmapRequestHeader hdr {};
    const auto                     view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(hdr));
    if (view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint request payload not addressable in stream");
        return false;
    }
    std::memcpy(&hdr, view.address(0), sizeof(hdr));

    // Step 2: protocol checks. Unmap has no response payload, so every
    // per-call failure is also session-fatal — there is no VkResult to surface
    // back to the application (real Vulkan vkUnmapMemory returns void).
    if (hdr.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint manager_revision mismatch ({} vs {})", hdr.manager_revision,
                        kMemoryMapManagerRevision);
        return false;
    }

    // Step 3: Phase 1 N2 invariant — unmap carries no MemoryTransferRange
    // entries. Phase 3a's coherent C2.1 path will lift this to range_count == 1
    // for the bracketed copy; until then anything else is a wire-format bug.
    if (hdr.range_count != 0) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint range_count={} not supported in Phase 1 (N2 expects 0)",
                        hdr.range_count);
        return false;
    }

    // Step 4: translate source-visible handles to receiver-native ones. The
    // source process never sees the receiver's real Vulkan handles, so unmap()
    // arrives carrying source values that mean nothing to the local driver.
    const auto device_entry = replay_context.source_to_receiver_device.find(hdr.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(hdr.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(hdr.device), static_cast<const void *>(hdr.memory));
        return false;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    // Step 5: drive the local driver. Real vkUnmapMemory returns void, so
    // there is no driver-side error to propagate. A null PFN is still a
    // protocol error (dispatch table was never primed).
    auto * const unmap_memory = replay_context.dispatch.device.unmap_memory;
    if (unmap_memory == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::unmap_endpoint dispatch table missing vkUnmapMemory");
        return false;
    }
    unmap_memory(receiver_device, receiver_memory);

    // Step 6: clear the private mapping state so a future map_endpoint on the
    // same allocation starts from a clean slate. receiver_mapped_ptr_ is the
    // only piece of state that uniquely identifies "currently mapped"; the
    // offsets are derived bookkeeping for Phase 2 flush/invalidate.
    receiver_mapped_ptr_ = nullptr;
    mapped_offset_       = 0;
    mapped_size_         = 0;

    // Step 7: unmap has no response payload — leave response_stream untouched.
    return true;
}

namespace {

// Per-call protocol-error response pack for flush. Mirrors the map_endpoint
// pattern: failures from this layer (revision mismatch, unknown handle)
// surface a VkResult to the source rather than tearing the session down.
void pack_flush_response(::vkfwd::CommandStream & response_stream, const wire::MemoryFlushResponse & response) {
    auto destination = response_stream.grow<std::uint8_t>(sizeof(response), alignof(wire::MemoryFlushResponse));
    std::memcpy(destination.address(0), &response, sizeof(response));
}

} // namespace

bool NonCoherentReceiverAllocation::flush_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range & request_range,
                                                   ::vkfwd::CommandStream & response_stream, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryFlushRequestHeader. The chunk layout mirrors the
    // forwarder-side append in non_coherent_allocation.cpp; payload alignment
    // is the request header's, then per-range entries, then byte payloads.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryFlushRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryFlushRequestHeader);
    if (request_range.size < kRangeOffset) {
        // Chunk too short to contain its declared payload — session-fatal.
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint chunk too small ({} bytes, need at least {})", request_range.size,
                        kRangeOffset);
        return false;
    }
    const auto header_view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(wire::MemoryFlushRequestHeader));
    if (header_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint request header not addressable");
        return false;
    }
    wire::MemoryFlushRequestHeader req {};
    std::memcpy(&req, header_view.address(0), sizeof(req));

    // Step 2: per-call protocol errors pack a response and return true.
    if (req.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint manager_revision mismatch ({} vs {})", req.manager_revision,
                        kMemoryMapManagerRevision);
        pack_flush_response(response_stream, wire::MemoryFlushResponse {
                                                 .manager_revision = kMemoryMapManagerRevision,
                                                 .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                             });
        return true;
    }

    // Step 3: translate source-visible handles. A missing entry indicates the
    // forwarder dispatched to a never-mapped allocation — session-fatal would
    // be heavy-handed, so we surface a per-call error.
    const auto device_entry = replay_context.source_to_receiver_device.find(req.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(req.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(req.device), static_cast<const void *>(req.memory));
        pack_flush_response(response_stream, wire::MemoryFlushResponse {
                                                 .manager_revision = kMemoryMapManagerRevision,
                                                 .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                             });
        return true;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    // Step 4: validate that we actually have a mapped pointer to copy into.
    // The forwarder side guards against this too, but a misordered chunk
    // (flush before map) would otherwise dereference a null pointer here.
    if (receiver_mapped_ptr_ == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint called on unmapped allocation");
        pack_flush_response(response_stream, wire::MemoryFlushResponse {
                                                 .manager_revision = kMemoryMapManagerRevision,
                                                 .return_value     = static_cast<std::int32_t>(VK_ERROR_MEMORY_MAP_FAILED),
                                             });
        return true;
    }

    // Step 5: walk each range. Copy bytes into the mapped pointer, build a
    // matching VkMappedMemoryRange[] in one allocation, then make a single
    // vkFlushMappedMemoryRanges call. Building the array bottom-up keeps the
    // driver call to one PFN invocation per chunk regardless of range_count.
    std::vector<VkMappedMemoryRange> driver_ranges;
    driver_ranges.reserve(req.range_count);
    for (std::uint32_t i = 0; i < req.range_count; ++i) {
        const std::size_t range_offset_in_chunk = kRangeOffset + static_cast<std::size_t>(i) * sizeof(wire::MemoryTransferRange);
        if (request_range.size < range_offset_in_chunk + sizeof(wire::MemoryTransferRange)) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint chunk truncated at range[{}] (chunk_size={})", i,
                            request_range.size);
            return false;
        }
        const auto range_view = request_stream.at(request_range.offset + range_offset_in_chunk, sizeof(wire::MemoryTransferRange));
        if (range_view.empty()) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint range[{}] not addressable", i);
            return false;
        }
        wire::MemoryTransferRange range_entry {};
        std::memcpy(&range_entry, range_view.address(0), sizeof(range_entry));

        // Payload bounds: the byte payload must lie inside the chunk and the
        // destination must lie inside the mapped extent. The forwarder always
        // atom-aligns within the mapped extent, but a hostile or buggy peer
        // could ship a range claiming to write past it.
        if (range_entry.offset < mapped_offset_ || range_entry.offset + range_entry.size > mapped_offset_ + mapped_size_ ||
            range_entry.offset + range_entry.size < range_entry.offset) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint range[{}] [{},{}) outside mapped [{},{})", i, range_entry.offset,
                            range_entry.offset + range_entry.size, mapped_offset_, mapped_offset_ + mapped_size_);
            return false;
        }
        const std::size_t payload_offset_in_chunk = static_cast<std::size_t>(range_entry.payload_offset);
        if (payload_offset_in_chunk < kRangeOffset + req.range_count * sizeof(wire::MemoryTransferRange) ||
            payload_offset_in_chunk + range_entry.size > request_range.size || payload_offset_in_chunk + range_entry.size < payload_offset_in_chunk) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint range[{}] payload at {} +{} outside chunk (size={})", i,
                            payload_offset_in_chunk, range_entry.size, request_range.size);
            return false;
        }
        const auto payload_view = request_stream.at(request_range.offset + payload_offset_in_chunk, static_cast<std::size_t>(range_entry.size));
        if (payload_view.empty() && range_entry.size != 0) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint range[{}] payload not addressable", i);
            return false;
        }

        // Mapped pointer addressing: receiver_mapped_ptr_ is the driver's
        // return for vkMapMemory(offset = mapped_offset_, size = mapped_size_).
        // So (range.offset - mapped_offset_) is the byte index inside the
        // mapped span.
        std::memcpy(static_cast<std::uint8_t *>(receiver_mapped_ptr_) + (range_entry.offset - mapped_offset_), payload_view.address(0),
                    static_cast<std::size_t>(range_entry.size));

        driver_ranges.push_back(VkMappedMemoryRange {
            .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext  = nullptr,
            .memory = receiver_memory,
            .offset = range_entry.offset,
            .size   = range_entry.size,
        });
    }

    // Step 6: drive the local driver. The forwarder pre-atom-aligned every
    // range, so the driver sees only spec-compliant VkMappedMemoryRange entries.
    VkResult driver_result = VK_SUCCESS;
    if (!driver_ranges.empty()) {
        auto * const pfn = replay_context.dispatch.device.flush_mapped_memory_ranges;
        if (pfn == nullptr) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::flush_endpoint dispatch table missing vkFlushMappedMemoryRanges");
            pack_flush_response(response_stream, wire::MemoryFlushResponse {
                                                     .manager_revision = kMemoryMapManagerRevision,
                                                     .return_value     = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                                 });
            return true;
        }
        driver_result = pfn(receiver_device, static_cast<std::uint32_t>(driver_ranges.size()), driver_ranges.data());
    }

    pack_flush_response(response_stream, wire::MemoryFlushResponse {
                                             .manager_revision = kMemoryMapManagerRevision,
                                             .return_value     = static_cast<std::int32_t>(driver_result),
                                         });
    return true;
}

bool NonCoherentReceiverAllocation::invalidate_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range & request_range,
                                                        ::vkfwd::CommandStream & response_stream, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryInvalidateRequestHeader. The chunk layout mirrors
    // the forwarder side; per-range entries are MemoryInvalidateRangeRequest
    // (no payload_offset since bytes flow on the response stream).
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryInvalidateRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryInvalidateRequestHeader);

    // Pre-pack a response header now and patch its return_value / range_count
    // before the final write. Doing it this way keeps the response-stream
    // layout (header + ranges + bytes) constructed in physical order so the
    // forwarder's at<...>(0, ...) lookups read a single contiguous prefix.
    auto header_destination   = response_stream.grow<std::uint8_t>(sizeof(wire::MemoryInvalidateResponseHeader), alignof(wire::MemoryInvalidateResponseHeader));
    auto pack_response_header = [&](std::int32_t return_value, std::uint32_t range_count) {
        const wire::MemoryInvalidateResponseHeader response {
            .manager_revision = kMemoryMapManagerRevision,
            .return_value     = return_value,
            .range_count      = range_count,
            .pad0             = 0,
        };
        std::memcpy(header_destination.address(0), &response, sizeof(response));
    };

    if (request_range.size < kRangeOffset) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint chunk too small ({} bytes, need at least {})", request_range.size,
                        kRangeOffset);
        return false;
    }
    const auto header_view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(wire::MemoryInvalidateRequestHeader));
    if (header_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint request header not addressable");
        return false;
    }
    wire::MemoryInvalidateRequestHeader req {};
    std::memcpy(&req, header_view.address(0), sizeof(req));

    if (req.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint manager_revision mismatch ({} vs {})", req.manager_revision,
                        kMemoryMapManagerRevision);
        pack_response_header(static_cast<std::int32_t>(VK_ERROR_UNKNOWN), 0);
        return true;
    }

    const auto device_entry = replay_context.source_to_receiver_device.find(req.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(req.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(req.device), static_cast<const void *>(req.memory));
        pack_response_header(static_cast<std::int32_t>(VK_ERROR_UNKNOWN), 0);
        return true;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    if (receiver_mapped_ptr_ == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint called on unmapped allocation");
        pack_response_header(static_cast<std::int32_t>(VK_ERROR_MEMORY_MAP_FAILED), 0);
        return true;
    }

    // Step 2: read every range, build the VkMappedMemoryRange[] for the
    // driver call, and remember our own atom-aligned ranges so we can copy
    // bytes back after invalidating.
    std::vector<VkMappedMemoryRange>                driver_ranges;
    std::vector<wire::MemoryInvalidateRangeRequest> request_ranges;
    driver_ranges.reserve(req.range_count);
    request_ranges.reserve(req.range_count);
    for (std::uint32_t i = 0; i < req.range_count; ++i) {
        const std::size_t range_offset_in_chunk = kRangeOffset + static_cast<std::size_t>(i) * sizeof(wire::MemoryInvalidateRangeRequest);
        if (request_range.size < range_offset_in_chunk + sizeof(wire::MemoryInvalidateRangeRequest)) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint chunk truncated at range[{}] (chunk_size={})", i,
                            request_range.size);
            return false;
        }
        const auto range_view = request_stream.at(request_range.offset + range_offset_in_chunk, sizeof(wire::MemoryInvalidateRangeRequest));
        if (range_view.empty()) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint range[{}] not addressable", i);
            return false;
        }
        wire::MemoryInvalidateRangeRequest range_entry {};
        std::memcpy(&range_entry, range_view.address(0), sizeof(range_entry));

        if (range_entry.offset < mapped_offset_ || range_entry.offset + range_entry.size > mapped_offset_ + mapped_size_ ||
            range_entry.offset + range_entry.size < range_entry.offset) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint range[{}] [{},{}) outside mapped [{},{})", i,
                            range_entry.offset, range_entry.offset + range_entry.size, mapped_offset_, mapped_offset_ + mapped_size_);
            return false;
        }
        request_ranges.push_back(range_entry);
        driver_ranges.push_back(VkMappedMemoryRange {
            .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .pNext  = nullptr,
            .memory = receiver_memory,
            .offset = range_entry.offset,
            .size   = range_entry.size,
        });
    }

    // Step 3: call real vkInvalidateMappedMemoryRanges FIRST. The whole point
    // of invalidate is to refresh the mapped pointer from device memory; if
    // we copied bytes out before this call, the driver would never have a
    // chance to populate them.
    VkResult driver_result = VK_SUCCESS;
    if (!driver_ranges.empty()) {
        auto * const pfn = replay_context.dispatch.device.invalidate_mapped_memory_ranges;
        if (pfn == nullptr) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint dispatch table missing vkInvalidateMappedMemoryRanges");
            pack_response_header(static_cast<std::int32_t>(VK_ERROR_UNKNOWN), 0);
            return true;
        }
        driver_result = pfn(receiver_device, static_cast<std::uint32_t>(driver_ranges.size()), driver_ranges.data());
    }

    // Step 4: append the MemoryTransferRange[] entries. The forwarder reads
    // the array immediately after the header at kHeaderSize (compile-time
    // checked at the forwarder), so we must grow in this exact order.
    for (std::uint32_t i = 0; i < req.range_count; ++i) {
        auto range_destination = response_stream.grow<std::uint8_t>(sizeof(wire::MemoryTransferRange), alignof(wire::MemoryTransferRange));
        const wire::MemoryTransferRange echoed_range {
            .offset = request_ranges[i].offset,
            .size   = request_ranges[i].size,
            // payload_offset is filled in below after we grow the byte
            // payload — appending out of order would invert the layout.
            .payload_offset = 0,
        };
        std::memcpy(range_destination.address(0), &echoed_range, sizeof(echoed_range));
    }

    // Step 5: append the byte payloads and patch each range's payload_offset.
    // The forwarder reads range[i].payload_offset as a response-stream offset,
    // so we capture the offset returned by grow() and rewrite the field.
    for (std::uint32_t i = 0; i < req.range_count; ++i) {
        const VkDeviceSize size_for_range      = request_ranges[i].size;
        std::size_t        payload_offset      = 0;
        auto               payload_destination = response_stream.grow<std::uint8_t>(static_cast<std::size_t>(size_for_range), 1, &payload_offset);
        if (size_for_range != 0) {
            // Same (range.offset - mapped_offset_) math as flush_endpoint: the
            // mapped pointer is the receiver-driver's vkMapMemory return value
            // and that maps from mapped_offset_.
            std::memcpy(payload_destination.address(0), static_cast<const std::uint8_t *>(receiver_mapped_ptr_) + (request_ranges[i].offset - mapped_offset_),
                        static_cast<std::size_t>(size_for_range));
        }

        // Re-read the previously-written MemoryTransferRange to patch the
        // payload_offset field. The header occupies the very first
        // sizeof(MemoryInvalidateResponseHeader) bytes; ranges follow back
        // to back; payloads follow ranges.
        constexpr std::size_t kHeaderSize  = sizeof(wire::MemoryInvalidateResponseHeader);
        const std::size_t     range_offset = kHeaderSize + static_cast<std::size_t>(i) * sizeof(wire::MemoryTransferRange);
        auto                  range_view   = response_stream.at<wire::MemoryTransferRange>(range_offset, sizeof(wire::MemoryTransferRange));
        if (range_view.empty()) {
            VKFWD_LOG_ERROR("vkfwd receiver: NonCoherentReceiverAllocation::invalidate_endpoint response range[{}] disappeared during patch", i);
            return false;
        }
        wire::MemoryTransferRange patched {};
        std::memcpy(&patched, range_view.address(0), sizeof(patched));
        patched.payload_offset = payload_offset;
        std::memcpy(range_view.address(0), &patched, sizeof(patched));
    }

    pack_response_header(static_cast<std::int32_t>(driver_result), req.range_count);
    return true;
}

} // namespace vkfwd::memory_map
