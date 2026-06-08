#include "memory_map/memory_info_fallback.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "forwarder.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/manual_chunk.hpp"
#include "memory_map/memory_type_registry.hpp"
#include "memory_map/wire_format.hpp"

#include <cstring>

namespace vkfwd::memory_map {

void request_memory_info_fallback(VkDevice device) {
    // Step 1: a device with no recorded vkCreateDevice cannot identify a
    // physical device, so we can't ask the receiver for memory properties at
    // all. The allocation stays untracked; this is not an error, just the
    // expected outcome for handles minted before any Vulkan instance/device
    // setup we observed.
    auto & registry        = MemoryTypeRegistry::instance();
    auto   physical_device = registry.physical_device_for(device);
    if (!physical_device || *physical_device == VK_NULL_HANDLE) {
        VKFWD_LOG_ERROR("vkfwd: memory_info_fallback skipped, no physical_device for device={}", static_cast<void *>(device));
        return;
    }

    // Step 2: build + append the manual chunk. Anything that prevents the
    // chunk from reaching the wire is fatal for this fallback only; the
    // caller's vkAllocateMemory still succeeds because the allocation handle
    // was returned by the receiver before this hook ran.
    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    const wire::QueryPhysicalDeviceMemoryInfoRequest request {
        .manager_revision = kMemoryMapManagerRevision,
        .pad0             = 0,
        .physical_device  = *physical_device,
    };
    const VkResult append_result = append_manual_chunk(stream, ::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo, request);
    if (append_result != VK_SUCCESS) {
        VKFWD_LOG_ERROR("vkfwd: memory_info_fallback append_manual_chunk failed, result={}", static_cast<std::int32_t>(append_result));
        return;
    }

    // Step 3: synchronous round-trip. The response always lands at offset 0 of
    // the returned stream (this helper sends exactly one chunk, so the
    // receiver writes a single response).
    ::vkfwd::CommandStream response_stream = forwarder.flush();

    auto response_view = response_stream.at<wire::QueryPhysicalDeviceMemoryInfoResponse>(0, sizeof(wire::QueryPhysicalDeviceMemoryInfoResponse));
    if (response_view.empty()) {
        VKFWD_LOG_ERROR("vkfwd: memory_info_fallback response stream too small ({} bytes)", response_stream.size());
        return;
    }
    wire::QueryPhysicalDeviceMemoryInfoResponse response {};
    std::memcpy(&response, response_view.address(0), sizeof(response));

    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: memory_info_fallback response manager_revision mismatch ({} vs {})", response.manager_revision, kMemoryMapManagerRevision);
        return;
    }
    if (response.return_value != VK_SUCCESS) {
        // Receiver-rejected query: the allocation stays untracked. We do not
        // partially populate the registry — a recorded memory_properties with
        // garbage values would silently change which forwarder-allocation
        // strategy gets picked for future allocations on the same physical
        // device.
        VKFWD_LOG_ERROR("vkfwd: memory_info_fallback receiver returned non-success ({})", response.return_value);
        return;
    }

    // Step 4: feed the response into the registry. record_* are idempotent
    // (just overwrite the cached value), so a redundant fallback call after a
    // successful one is harmless — the second resolve() will hit cache.
    registry.record_memory_properties(*physical_device, response.memory_properties);
    registry.record_non_coherent_atom_size(*physical_device, response.non_coherent_atom_size);
    registry.record_min_memory_map_alignment(*physical_device, static_cast<std::size_t>(response.min_memory_map_alignment));
}

} // namespace vkfwd::memory_map
