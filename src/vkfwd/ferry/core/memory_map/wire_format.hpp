#pragma once

#include "memory_map/manager.hpp" // kMemoryMapManagerRevision

#include <vulkan/vulkan.h>

#include <cstdint>
#include <type_traits>

namespace vkfwd::memory_map::wire {

// All wire structs are trivially-copyable POD so the receiver can read them by
// memcpy from the request stream. Every struct embeds manager_revision so a
// mismatched session is detected on the very first chunk.

struct MemoryMapRequest {
    std::uint32_t    manager_revision = kMemoryMapManagerRevision;
    std::uint32_t    pad0             = 0; // alignment placeholder so 64-bit fields below align.
    VkDevice         device           = VK_NULL_HANDLE;
    VkDeviceMemory   memory           = VK_NULL_HANDLE;
    VkDeviceSize     offset           = 0;
    VkDeviceSize     size             = 0;
    VkMemoryMapFlags flags            = 0;
    std::uint32_t    pad1             = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapRequest>);
// Lock the wire size: 4+4 + 8+8 + 8+8 + 4+4 = 48 bytes. A future field addition
// must update this assertion AND bump kMemoryMapManagerRevision; otherwise the
// receiver silently misinterprets the payload.
static_assert(sizeof(MemoryMapRequest) == 48);

struct MemoryMapResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
    VkDeviceSize  effective_size   = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapResponse>);
static_assert(sizeof(MemoryMapResponse) == 16);

struct MemoryUnmapRequestHeader {
    std::uint32_t  manager_revision = kMemoryMapManagerRevision;
    std::uint32_t  range_count      = 0;
    VkDevice       device           = VK_NULL_HANDLE;
    VkDeviceMemory memory           = VK_NULL_HANDLE;
    VkDeviceSize   mapped_offset    = 0;
    VkDeviceSize   mapped_size      = 0;
    std::uint32_t  reserved         = 0;
    std::uint32_t  pad0             = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryUnmapRequestHeader>);
static_assert(sizeof(MemoryUnmapRequestHeader) == 48);

// MemoryUnmapRequestHeader is followed by `range_count` of these, then the
// raw byte payloads each MemoryTransferRange.payload_offset points to. Phase 1
// always emits range_count == 0; Phase 3a's coherent unmap emits range_count == 1
// for the whole mapped range.
struct MemoryTransferRange {
    VkDeviceSize  offset         = 0; // allocation-relative
    VkDeviceSize  size           = 0;
    std::uint64_t payload_offset = 0; // relative to start of command chunk
};
static_assert(std::is_trivially_copyable_v<MemoryTransferRange>);

} // namespace vkfwd::memory_map::wire
