#include "memory_map/receiver/coherent_allocation.hpp"

#include "command_stream.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/wire_format.hpp"
#include "replay_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vkfwd::memory_map {

namespace {

// Append a MemoryMapResponse at the very front of the response stream. The
// forwarder reads via at<MemoryMapResponse>(0, ...) so the first grow on an
// empty stream must land at offset 0; kBaseAlignment (128) is a multiple of
// alignof(MemoryMapResponse), so the smaller alignment requested here still
// produces offset 0 on a fresh stream — identical to the non-coherent helper.
void pack_response(::vkfwd::CommandStream & response_stream, const wire::MemoryMapResponse & response) {
    auto destination = response_stream.grow<std::uint8_t>(sizeof(response), alignof(wire::MemoryMapResponse));
    std::memcpy(destination.address(0), &response, sizeof(response));
}

} // namespace

bool CoherentReceiverAllocation::map_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range & request_range,
                                              ::vkfwd::CommandStream & response_stream, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryMapRequest. Layout mirrors the non-coherent
    // map_endpoint; the chunk header is followed by the request struct at
    // its natural alignment, exactly what append_manual_chunk produces.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryMapRequest);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    if (request_range.size < kPayloadOffset + sizeof(wire::MemoryMapRequest)) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::map_endpoint chunk too small ({} bytes, need {})", request_range.size,
                        kPayloadOffset + sizeof(wire::MemoryMapRequest));
        return false;
    }
    wire::MemoryMapRequest req {};
    const auto             view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(req));
    if (view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::map_endpoint request payload not addressable in stream");
        return false;
    }
    std::memcpy(&req, view.address(0), sizeof(req));

    // Step 2: per-call failures pack a response and return true so the
    // session keeps running. Only stream-level corruption returns false.
    if (req.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::map_endpoint request manager_revision mismatch ({} vs {})", req.manager_revision,
                        kMemoryMapManagerRevision);
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision        = kMemoryMapManagerRevision,
                                           .return_value            = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size          = 0,
                                           .initial_payload_present = 0,
                                           .payload_offset          = 0,
                                       });
        return true;
    }

    const auto device_entry = replay_context.source_to_receiver_device.find(req.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(req.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::map_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(req.device), static_cast<const void *>(req.memory));
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision        = kMemoryMapManagerRevision,
                                           .return_value            = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size          = 0,
                                           .initial_payload_present = 0,
                                           .payload_offset          = 0,
                                       });
        return true;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    const VkDeviceSize allocation_size = info().allocation_size;
    const VkDeviceSize effective_size  = (req.size == VK_WHOLE_SIZE) ? (allocation_size - req.offset) : req.size;

    auto * const map_memory = replay_context.dispatch.device.map_memory;
    if (map_memory == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::map_endpoint dispatch table missing vkMapMemory");
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision        = kMemoryMapManagerRevision,
                                           .return_value            = static_cast<std::int32_t>(VK_ERROR_UNKNOWN),
                                           .effective_size          = 0,
                                           .initial_payload_present = 0,
                                           .payload_offset          = 0,
                                       });
        return true;
    }
    void *         receiver_ptr  = nullptr;
    const VkResult driver_result = map_memory(receiver_device, receiver_memory, req.offset, effective_size, req.flags, &receiver_ptr);
    if (driver_result != VK_SUCCESS) {
        pack_response(response_stream, wire::MemoryMapResponse {
                                           .manager_revision        = kMemoryMapManagerRevision,
                                           .return_value            = static_cast<std::int32_t>(driver_result),
                                           .effective_size          = 0,
                                           .initial_payload_present = 0,
                                           .payload_offset          = 0,
                                       });
        return true;
    }

    // Step 3: success — record the mapping so unmap_endpoint has somewhere to
    // copy the source-supplied bytes back into.
    receiver_mapped_ptr_ = receiver_ptr;
    mapped_offset_       = req.offset;
    mapped_size_         = effective_size;

    // Step 4: C2.1 receiver→source bracketed copy. Pack the response header
    // FIRST so it lives at response_stream offset 0 (forwarder's at<>(0)
    // requirement), then grow the payload bytes. The payload's logical
    // offset is NOT necessarily sizeof(MemoryMapResponse): CommandStream
    // inserts a gap header and may spill into a new chunk when the payload
    // does not fit alongside the response. Capture the offset returned by
    // grow() and patch it into the response so the forwarder reads from the
    // correct location regardless of whether a gap was inserted.
    pack_response(response_stream, wire::MemoryMapResponse {
                                       .manager_revision        = kMemoryMapManagerRevision,
                                       .return_value            = static_cast<std::int32_t>(VK_SUCCESS),
                                       .effective_size          = effective_size,
                                       .initial_payload_present = 1,
                                       .payload_offset          = sizeof(wire::MemoryMapResponse), // provisional; patched below if grow() lands elsewhere
                                   });
    if (effective_size != 0) {
        // The bytes the driver wrote into the mapping (or, for a freshly
        // allocated coherent allocation, whatever undefined contents the
        // driver guarantees). The forwarder copies them into source staging
        // so a subsequent CPU read of ppData sees what the receiver sees.
        std::size_t payload_offset      = 0;
        auto        payload_destination = response_stream.grow<std::uint8_t>(static_cast<std::size_t>(effective_size), 1, &payload_offset);
        std::memcpy(payload_destination.address(0), static_cast<const std::uint8_t *>(receiver_ptr), static_cast<std::size_t>(effective_size));

        // Patch the response's payload_offset to the real logical offset.
        // The response header sits at offset 0; reading it back and
        // rewriting payload_offset is safer than assuming offset 0 since the
        // CommandStream::at<> typed accessor enforces alignment + addressability.
        auto response_view = response_stream.at<wire::MemoryMapResponse>(0, sizeof(wire::MemoryMapResponse));
        if (!response_view.empty()) {
            wire::MemoryMapResponse patched {};
            std::memcpy(&patched, response_view.address(0), sizeof(patched));
            patched.payload_offset = payload_offset;
            std::memcpy(response_view.address(0), &patched, sizeof(patched));
        }
    }
    return true;
}

bool CoherentReceiverAllocation::unmap_endpoint(const ::vkfwd::CommandStream & request_stream, const ::vkfwd::Range &            request_range,
                                                ::vkfwd::CommandStream & /*response_stream*/, ::vkfwd::receiver::ReplayContext & replay_context) {
    // Step 1: read MemoryUnmapRequestHeader. Layout matches the forwarder's
    // hand-rolled unmap chunk in CoherentForwarderAllocation::unmap:
    // chunk header + header (at alignof header) + range[1] + payload bytes.
    constexpr std::size_t kPayloadAlignment = alignof(wire::MemoryUnmapRequestHeader);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kRangeOffset      = kPayloadOffset + sizeof(wire::MemoryUnmapRequestHeader);
    if (request_range.size < kRangeOffset) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint chunk too small ({} bytes, need at least {})", request_range.size,
                        kRangeOffset);
        return false;
    }
    wire::MemoryUnmapRequestHeader hdr {};
    const auto                     header_view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(hdr));
    if (header_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint request header not addressable");
        return false;
    }
    std::memcpy(&hdr, header_view.address(0), sizeof(hdr));

    if (hdr.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint manager_revision mismatch ({} vs {})", hdr.manager_revision,
                        kMemoryMapManagerRevision);
        return false;
    }

    // C2.1 invariant: coherent unmap ALWAYS carries exactly one range. The
    // forwarder builds it that way; any other shape is a wire-format bug
    // (or a non-coherent unmap routed to the wrong subclass).
    if (hdr.range_count != 1) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint range_count={} not supported (C2.1 expects exactly 1)", hdr.range_count);
        return false;
    }

    const auto device_entry = replay_context.source_to_receiver_device.find(hdr.device);
    const auto memory_entry = replay_context.source_to_receiver_memory.find(hdr.memory);
    if (device_entry == replay_context.source_to_receiver_device.end() || memory_entry == replay_context.source_to_receiver_memory.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint unknown source handle (device={}, memory={})",
                        static_cast<const void *>(hdr.device), static_cast<const void *>(hdr.memory));
        return false;
    }
    VkDevice const       receiver_device = device_entry->second;
    VkDeviceMemory const receiver_memory = memory_entry->second;

    if (receiver_mapped_ptr_ == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint called on unmapped allocation");
        return false;
    }

    // Step 2: read the single MemoryTransferRange and validate it against
    // the actively mapped extent. A hostile peer could ship a range that
    // claims to write past the mapping; that would splatter receiver memory.
    if (request_range.size < kRangeOffset + sizeof(wire::MemoryTransferRange)) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint chunk truncated at range (chunk_size={})", request_range.size);
        return false;
    }
    const auto range_view = request_stream.at(request_range.offset + kRangeOffset, sizeof(wire::MemoryTransferRange));
    if (range_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint range not addressable");
        return false;
    }
    wire::MemoryTransferRange range {};
    std::memcpy(&range, range_view.address(0), sizeof(range));

    if (range.offset < mapped_offset_ || range.offset + range.size > mapped_offset_ + mapped_size_ || range.offset + range.size < range.offset) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint range [{},{}) outside mapped [{},{})", range.offset,
                        range.offset + range.size, mapped_offset_, mapped_offset_ + mapped_size_);
        return false;
    }

    // Step 3: copy source-supplied bytes into the mapped pointer BEFORE the
    // vkUnmapMemory call. After unmap the driver may reuse the pages, so
    // writing after-unmap would be a use-after-free. Coherent memory means
    // the device sees these writes immediately — no vkFlushMappedMemoryRanges
    // needed (and the spec says it would be a no-op anyway).
    const std::size_t payload_offset_in_chunk = static_cast<std::size_t>(range.payload_offset);
    if (payload_offset_in_chunk < kRangeOffset + sizeof(wire::MemoryTransferRange) || payload_offset_in_chunk + range.size > request_range.size ||
        payload_offset_in_chunk + range.size < payload_offset_in_chunk) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint payload at {} +{} outside chunk (size={})", payload_offset_in_chunk,
                        range.size, request_range.size);
        return false;
    }
    const auto payload_view = request_stream.at(request_range.offset + payload_offset_in_chunk, static_cast<std::size_t>(range.size));
    if (payload_view.empty() && range.size != 0) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint payload not addressable");
        return false;
    }
    if (range.size != 0) {
        // (range.offset - mapped_offset_) is the byte index inside the mapped
        // span — same math as flush_endpoint on the non-coherent side, for
        // the same reason: vkMapMemory's return points at mapped_offset_.
        std::memcpy(static_cast<std::uint8_t *>(receiver_mapped_ptr_) + (range.offset - mapped_offset_), payload_view.address(0),
                    static_cast<std::size_t>(range.size));
    }

    // Step 4: drive the local driver. Real vkUnmapMemory returns void, so a
    // null PFN is the only failure to surface (and it must be a session-
    // level error — a coherent unmap has no response payload to carry a
    // per-call VkResult back to the source).
    auto * const unmap_memory = replay_context.dispatch.device.unmap_memory;
    if (unmap_memory == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: CoherentReceiverAllocation::unmap_endpoint dispatch table missing vkUnmapMemory");
        return false;
    }
    unmap_memory(receiver_device, receiver_memory);

    receiver_mapped_ptr_ = nullptr;
    mapped_offset_       = 0;
    mapped_size_         = 0;
    return true;
}

// Coherent memory's flush/invalidate are spec no-ops, and the forwarder
// returns VK_ERROR_FEATURE_NOT_PRESENT before any chunk reaches the wire.
// If a chunk shows up here anyway, the only way that could happen is a
// wire-format bug or a misclassified allocation — treat it as a stream
// error (return false) so ReceiverSession aborts the stream and surfaces
// the divergence loudly. The same reasoning that justifies the forwarder
// stub applies here.
bool CoherentReceiverAllocation::flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR(
        "vkfwd receiver: CoherentReceiverAllocation::flush_endpoint should never receive a chunk (spec says flush is a no-op for coherent memory; forwarder "
        "rejects with VK_ERROR_FEATURE_NOT_PRESENT)");
    return false;
}

bool CoherentReceiverAllocation::invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR(
        "vkfwd receiver: CoherentReceiverAllocation::invalidate_endpoint should never receive a chunk (spec says invalidate is a no-op for coherent memory; "
        "forwarder rejects with VK_ERROR_FEATURE_NOT_PRESENT)");
    return false;
}

} // namespace vkfwd::memory_map
