#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace vkfwd::receiver {
struct ReplayContext;
}

namespace vkfwd {

class CommandStream;
struct Range;

// Protocol revision for the staged map/unmap wire format.
// Increment when the custom memory-map request/response layouts or
// staging-transfer behavior changes. Both forwarder and receiver must agree.
constexpr std::uint32_t kMemoryMapManagerRevision = 1;

// Forwarder-side singleton. The public Vulkan vkMapMemory/vkUnmapMemory
// entry points delegate here only when they are sending vkfwd's custom
// memory-map wire commands, not the generated Vulkan command payloads.
// Thread-safe across concurrent calls on different VkDeviceMemory handles.
// The caller is responsible for external synchronization on the same
// VkDeviceMemory as required by the Vulkan spec.
class MemoryMapForwarder {
public:
    static MemoryMapForwarder & instance();

    // record_allocation captures everything a ForwarderAllocation subclass
    // needs to construct itself. The allocate hook resolves property_flags,
    // memory_type_index, non_coherent_atom_size, and min_memory_map_alignment
    // through MemoryTypeRegistry before calling here so the manager itself
    // does not depend on the registry.
    void record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment);

    void forget_allocation(VkDeviceMemory memory);

    VkResult custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData);

    void custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory);

    // vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges public Vulkan
    // entry points delegate here. The custom_ names match the generator's
    // FORWARDER_MEMORY_MAP_MANAGED_COMMANDS template; flush_ranges /
    // invalidate_ranges are the underlying per-range dispatchers.
    VkResult custom_vkFlushMappedMemoryRanges_entry(VkDevice device, std::uint32_t memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges);
    VkResult custom_vkInvalidateMappedMemoryRanges_entry(VkDevice device, std::uint32_t memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges);
    VkResult flush_ranges(VkDevice device, std::uint32_t range_count, const VkMappedMemoryRange * ranges);
    VkResult invalidate_ranges(VkDevice device, std::uint32_t range_count, const VkMappedMemoryRange * ranges);

    /// @brief Test helper to query the recorded allocation size for a given memory handle. Not a public API.
    VkDeviceSize test_get_allocation_size(VkDeviceMemory memory) const;

private:
    MemoryMapForwarder();
    ~MemoryMapForwarder();

    class Impl;
    Impl * impl_ = nullptr;
};

// Receiver-side per-context instance. Stored in ReplayContext.
// Not thread-safe; each ReplayContext is owned by one source-thread stream.
class MemoryMapReceiver {
public:
    MemoryMapReceiver();
    ~MemoryMapReceiver();

    // Phase 0 leaves these uncalled by generated standard Vulkan endpoint
    // code. Phase 1 dispatches manual::CommandId::MemoryMap / MemoryUnmap
    // chunks to the custom endpoint methods below; the per-handle map gets
    // populated through that path, not through the generated standard
    // vkAllocateMemory endpoint.
    void record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment);
    void forget_allocation(VkDeviceMemory memory);

    // custom_* names intentional: these methods handle vkfwd's manual
    // MemoryMap / MemoryUnmap command ids, not standard generated Vulkan
    // CommandId::MapMemory / CommandId::UnmapMemory chunks. ReplayContext is
    // threaded through so endpoint bodies can reach the source->receiver handle
    // map and the receiver dispatch table without a back-pointer.
    bool custom_vkMapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                     ::vkfwd::receiver::ReplayContext & replay_context);

    bool custom_vkUnmapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                       ::vkfwd::receiver::ReplayContext & replay_context);

private:
    class Impl;
    Impl * impl_ = nullptr;
};

} // namespace vkfwd
