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
    std::uint32_t    manager_revision  = kMemoryMapManagerRevision;
    std::uint32_t    memory_type_index = 0;
    VkDevice         device            = VK_NULL_HANDLE;
    VkDeviceMemory   memory            = VK_NULL_HANDLE;
    VkDeviceSize     offset            = 0;
    VkDeviceSize     size              = 0; // VK_WHOLE_SIZE allowed; receiver re-resolves and validates.
    VkMemoryMapFlags flags             = 0;
    std::uint32_t    pad0              = 0;
    // Forwarder-resolved classification carried so the receiver can lazily
    // construct the matching ReceiverAllocation strategy without its own
    // MemoryTypeRegistry. Stored on every map request (not just the first) so
    // the receiver does not need to track "have I created the allocation yet".
    VkMemoryPropertyFlags property_flags           = 0;
    std::uint32_t         pad1                     = 0;
    VkDeviceSize          allocation_size          = 0;
    VkDeviceSize          non_coherent_atom_size   = 0;
    std::uint64_t         min_memory_map_alignment = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapRequest>);
// Lock the wire size: 4+4 + 8+8 + 8+8 + 4+4 + 4+4 + 8+8+8 = 80 bytes. Future
// field additions must update this assertion AND bump kMemoryMapManagerRevision.
static_assert(sizeof(MemoryMapRequest) == 80);

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
    std::uint64_t payload_offset = 0; // relative to start of command chunk (request) or response stream
};
static_assert(std::is_trivially_copyable_v<MemoryTransferRange>);
static_assert(sizeof(MemoryTransferRange) == 24);

// Forwarder-initiated fallback when MemoryTypeRegistry cannot resolve a
// (device, memoryTypeIndex) at vkAllocateMemory time. The receiver answers
// with the receiver-driver's authoritative VkPhysicalDeviceMemoryProperties +
// the two device limits the forwarder needs to construct a ForwarderAllocation
// that satisfies Vulkan's mapped-pointer alignment contract.
struct QueryPhysicalDeviceMemoryInfoRequest {
    std::uint32_t    manager_revision = kMemoryMapManagerRevision;
    std::uint32_t    pad0             = 0;
    VkPhysicalDevice physical_device  = VK_NULL_HANDLE;
};
static_assert(std::is_trivially_copyable_v<QueryPhysicalDeviceMemoryInfoRequest>);
static_assert(sizeof(QueryPhysicalDeviceMemoryInfoRequest) == 16);

struct QueryPhysicalDeviceMemoryInfoResponse {
    std::uint32_t                    manager_revision = kMemoryMapManagerRevision;
    std::int32_t                     return_value     = VK_SUCCESS;
    VkPhysicalDeviceMemoryProperties memory_properties {};
    VkDeviceSize                     non_coherent_atom_size   = 0;
    std::uint64_t                    min_memory_map_alignment = 0;
};
static_assert(std::is_trivially_copyable_v<QueryPhysicalDeviceMemoryInfoResponse>);
// Intentionally no sizeof assertion: VkPhysicalDeviceMemoryProperties size
// derives from VK_MAX_MEMORY_TYPES/VK_MAX_MEMORY_HEAPS, which are compile-time
// constants but vary across Vulkan SDK versions. Trivial-copyability plus
// matching SDK on both sides is the load-bearing invariant.

// ---- Phase 2: flush / invalidate ------------------------------------------
//
// Both flush and invalidate are per-range transfers under N2 (no synced-range
// tracking; each call is self-contained). The forwarder sends one chunk per
// `MemoryMapForwarder::flush_ranges` / `invalidate_ranges` iteration, even
// though the wire formats below carry a `range_count` so a future batched
// implementation can reuse the same layout without bumping the revision.

// Flush: source bytes flow to the receiver. The chunk layout is
//   [ CommandChunkHeader ]
//   [ MemoryFlushRequestHeader ]
//   [ MemoryTransferRange[range_count] ]   // payload_offset is chunk-relative
//   [ raw bytes ]
// The receiver writes each range's bytes into the mapped pointer, then calls
// real vkFlushMappedMemoryRanges so the GPU sees the host writes.
struct MemoryFlushRequestHeader {
    std::uint32_t  manager_revision = kMemoryMapManagerRevision;
    std::uint32_t  range_count      = 0;
    VkDevice       device           = VK_NULL_HANDLE;
    VkDeviceMemory memory           = VK_NULL_HANDLE;
};
static_assert(std::is_trivially_copyable_v<MemoryFlushRequestHeader>);
static_assert(sizeof(MemoryFlushRequestHeader) == 24);

// Reuses MemoryTransferRange { offset, size, payload_offset } already defined
// above; payload_offset is relative to the start of the command chunk
// (consistent with the chunk-relative slot semantics described in CLAUDE.md
// for command parameter pointers).

struct MemoryFlushResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
};
static_assert(std::is_trivially_copyable_v<MemoryFlushResponse>);
static_assert(sizeof(MemoryFlushResponse) == 8);

// Invalidate: receiver bytes flow back to the source. The request only needs
// (offset, size) per range; the payload travels on the response. The receiver
// calls real vkInvalidateMappedMemoryRanges FIRST so the driver refreshes the
// mapped pointer from device memory, then it copies those bytes into the
// response stream.
struct MemoryInvalidateRequestHeader {
    std::uint32_t  manager_revision = kMemoryMapManagerRevision;
    std::uint32_t  range_count      = 0;
    VkDevice       device           = VK_NULL_HANDLE;
    VkDeviceMemory memory           = VK_NULL_HANDLE;
};
static_assert(std::is_trivially_copyable_v<MemoryInvalidateRequestHeader>);
static_assert(sizeof(MemoryInvalidateRequestHeader) == 24);

struct MemoryInvalidateRangeRequest {
    VkDeviceSize offset = 0;
    VkDeviceSize size   = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryInvalidateRangeRequest>);
static_assert(sizeof(MemoryInvalidateRangeRequest) == 16);

// Response stream layout for invalidate:
//   [ MemoryInvalidateResponseHeader ]
//   [ MemoryTransferRange[range_count] ]   // payload_offset is response-stream-relative
//   [ raw bytes ]
// pad0 keeps the struct multiple-of-8 sized so the trailing MemoryTransferRange
// array stays naturally aligned without an extra grow alignment dance.
struct MemoryInvalidateResponseHeader {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
    std::uint32_t range_count      = 0;
    std::uint32_t pad0             = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryInvalidateResponseHeader>);
static_assert(sizeof(MemoryInvalidateResponseHeader) == 16);

} // namespace vkfwd::memory_map::wire
