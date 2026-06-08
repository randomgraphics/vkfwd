#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

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

    void record_allocation(VkDevice device, VkDeviceMemory memory, VkDeviceSize allocation_size);

    void forget_allocation(VkDeviceMemory memory);

    VkResult custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData);

    void custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory);

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

    bool custom_vkMapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream);

    bool custom_vkUnmapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream);

private:
    class Impl;
    Impl * impl_ = nullptr;
};

} // namespace vkfwd
