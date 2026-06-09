#include "manual_dispatch.hpp"

#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/wire_format.hpp"
#include "replay_context.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vkfwd::receiver {

namespace {

// Append a QueryPhysicalDeviceMemoryInfoResponse at the very front of
// response_stream so the forwarder's at<>(0, ...) read finds it. The first
// grow on an empty stream lands at offset 0 because CommandStream::kBaseAlignment
// (128) is a multiple of the response's alignof.
void pack_query_response(::vkfwd::CommandStream & response_stream, const ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoResponse & response) {
    auto destination = response_stream.grow<std::uint8_t>(sizeof(response), alignof(::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoResponse));
    std::memcpy(destination.address(0), &response, sizeof(response));
}

bool dispatch_query_physical_device_memory_info(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                                ReplayContext & replay_context) {
    using ::vkfwd::kMemoryMapManagerRevision;
    using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoRequest;
    using ::vkfwd::memory_map::wire::QueryPhysicalDeviceMemoryInfoResponse;

    // Step 1: locate the request payload within the chunk. Mirror the
    // forwarder-side append_manual_chunk math exactly so any header-layout
    // change breaks both sides at the same revision boundary.
    constexpr std::size_t kPayloadAlignment = alignof(QueryPhysicalDeviceMemoryInfoRequest);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    if (request_range.size < kPayloadOffset + sizeof(QueryPhysicalDeviceMemoryInfoRequest)) {
        VKFWD_LOG_ERROR("vkfwd receiver: QueryPhysicalDeviceMemoryInfo chunk too small ({} bytes, need {})", request_range.size,
                        kPayloadOffset + sizeof(QueryPhysicalDeviceMemoryInfoRequest));
        return false;
    }
    QueryPhysicalDeviceMemoryInfoRequest req {};
    const auto                           view = request_stream.at(request_range.offset + kPayloadOffset, sizeof(req));
    if (view.empty()) {
        VKFWD_LOG_ERROR("vkfwd receiver: QueryPhysicalDeviceMemoryInfo request payload not addressable in stream");
        return false;
    }
    std::memcpy(&req, view.address(0), sizeof(req));

    // Per-call protocol errors after this point pack a non-success response
    // and return true so the session stays alive — only chunk-layout errors
    // are session-fatal.
    QueryPhysicalDeviceMemoryInfoResponse response {};
    response.manager_revision = kMemoryMapManagerRevision;

    if (req.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd receiver: QueryPhysicalDeviceMemoryInfo manager_revision mismatch ({} vs {})", req.manager_revision, kMemoryMapManagerRevision);
        response.return_value = static_cast<std::int32_t>(VK_ERROR_UNKNOWN);
        pack_query_response(response_stream, response);
        return true;
    }

    // Step 2: translate the source-visible VkPhysicalDevice to a
    // receiver-native handle. The source process never sees the receiver's
    // real handles, so the request carries a value that means nothing to the
    // local driver until we look it up here.
    const auto entry = replay_context.source_to_receiver_physical_device.find(req.physical_device);
    if (entry == replay_context.source_to_receiver_physical_device.end()) {
        VKFWD_LOG_ERROR("vkfwd receiver: QueryPhysicalDeviceMemoryInfo unknown source physical_device={}", static_cast<const void *>(req.physical_device));
        response.return_value = static_cast<std::int32_t>(VK_ERROR_UNKNOWN);
        pack_query_response(response_stream, response);
        return true;
    }
    VkPhysicalDevice const receiver_physical_device = entry->second;

    // Step 3: drive the local driver. Both PFNs are looked up at instance
    // dispatch init time, so a null here is a wiring bug not a runtime error;
    // surface it as VK_ERROR_UNKNOWN.
    auto * const memory_props_pfn = replay_context.dispatch.instance.get_physical_device_memory_properties;
    auto * const phys_props_pfn   = replay_context.dispatch.instance.get_physical_device_properties;
    if (memory_props_pfn == nullptr || phys_props_pfn == nullptr) {
        VKFWD_LOG_ERROR("vkfwd receiver: QueryPhysicalDeviceMemoryInfo dispatch table missing PFN (mem_set={}, props_set={})", memory_props_pfn != nullptr,
                        phys_props_pfn != nullptr);
        response.return_value = static_cast<std::int32_t>(VK_ERROR_UNKNOWN);
        pack_query_response(response_stream, response);
        return true;
    }

    VkPhysicalDeviceMemoryProperties memory_properties {};
    memory_props_pfn(receiver_physical_device, &memory_properties);

    VkPhysicalDeviceProperties device_properties {};
    phys_props_pfn(receiver_physical_device, &device_properties);

    response.return_value             = static_cast<std::int32_t>(VK_SUCCESS);
    response.memory_properties        = memory_properties;
    response.non_coherent_atom_size   = device_properties.limits.nonCoherentAtomSize;
    response.min_memory_map_alignment = device_properties.limits.minMemoryMapAlignment;
    pack_query_response(response_stream, response);
    return true;
}

} // namespace

bool dispatch_manual_command(::vkfwd::manual::CommandId command_id, const CommandStream & request_stream, const Range & request_range,
                             CommandStream & response_stream, ReplayContext & replay_context) {
    switch (command_id) {
    case ::vkfwd::manual::CommandId::MemoryMap:
        return replay_context.memoryMap.custom_vkMapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::MemoryUnmap:
        return replay_context.memoryMap.custom_vkUnmapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo:
        return dispatch_query_physical_device_memory_info(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::MemoryFlush:
        return replay_context.memoryMap.custom_vkFlushMappedMemoryRanges_endpoint(request_stream, request_range, response_stream, replay_context);
    case ::vkfwd::manual::CommandId::MemoryInvalidate:
        return replay_context.memoryMap.custom_vkInvalidateMappedMemoryRanges_endpoint(request_stream, request_range, response_stream, replay_context);
    }
    VKFWD_LOG_ERROR("vkfwd receiver: unknown manual command_id={}", static_cast<std::uint32_t>(command_id));
    return false;
}

} // namespace vkfwd::receiver
