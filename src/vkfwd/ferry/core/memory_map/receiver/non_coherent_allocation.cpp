#include "memory_map/receiver/non_coherent_allocation.hpp"

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

bool NonCoherentReceiverAllocation::flush_endpoint(const ::vkfwd::CommandStream &, const ::vkfwd::Range &, ::vkfwd::CommandStream &,
                                                   ::vkfwd::receiver::ReplayContext &) {
    return false;
}

bool NonCoherentReceiverAllocation::invalidate_endpoint(const ::vkfwd::CommandStream &, const ::vkfwd::Range &, ::vkfwd::CommandStream &,
                                                        ::vkfwd::receiver::ReplayContext &) {
    return false;
}

} // namespace vkfwd::memory_map
