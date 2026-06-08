#include "memory_map/manager.hpp"

#include "logging.hpp"

#include <mutex>
#include <unordered_map>

namespace vkfwd {

// ---- MemoryMapForwarder::Impl -----------------------------------------------

class MemoryMapForwarder::Impl {
public:
    struct AllocationRecord {
        VkDevice     device = VK_NULL_HANDLE;
        VkDeviceSize size   = 0;
    };

    void record_allocation(VkDevice device, VkDeviceMemory memory, VkDeviceSize allocation_size) {
        if (memory == VK_NULL_HANDLE) { return; }

        std::lock_guard lock(mutex);
        // vkMapMemory needs the original allocation extent to resolve
        // VK_WHOLE_SIZE without asking the receiver for another synchronous query.
        // The handle remains caller-visible after response copy-back, so this map
        // is keyed by the same handle the application will later pass to map/free.
        allocations[memory] = AllocationRecord {
            .device = device,
            .size   = allocation_size,
        };
    }

    void forget_allocation(VkDeviceMemory memory) {
        if (memory == VK_NULL_HANDLE) { return; }

        std::lock_guard lock(mutex);
        // Free is currently deferrable on the transport, but the source process
        // must stop accepting future maps for this handle as soon as the app frees
        // it. Receiver replay still observes the serialized vkFreeMemory in order.
        allocations.erase(memory);
    }

    VkDeviceSize test_get_allocation_size(VkDeviceMemory memory) const {
        std::lock_guard lock(mutex);
        const auto      found = allocations.find(memory);
        if (found == allocations.end()) { return 0; }
        return found->second.size;
    }

    VkResult custom_vkMapMemory_entry(VkDevice /*device*/, VkDeviceMemory /*memory*/, VkDeviceSize /*offset*/, VkDeviceSize /*size*/,
                                      VkMemoryMapFlags /*flags*/, void ** ppData) {
        // Placeholder: the custom memory-map wire protocol is not yet
        // implemented. Do not return a receiver-side mapped pointer through the
        // standard Vulkan response path; it is invalid in the source process.
        // Return a clear error so callers can detect the boundary rather than
        // crashing when they dereference a stale receiver-side pointer.
        if (ppData) { *ppData = nullptr; }
        VKFWD_LOG_ERROR("vkfwd MemoryMapForwarder: custom_vkMapMemory_entry not yet implemented (placeholder)");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    void custom_vkUnmapMemory_entry(VkDevice /*device*/, VkDeviceMemory /*memory*/) {
        // Placeholder: nothing to flush since vkMapMemory never succeeded.
    }

    mutable std::mutex                                   mutex;
    std::unordered_map<VkDeviceMemory, AllocationRecord> allocations;
};

// ---- MemoryMapForwarder -----------------------------------------------------

MemoryMapForwarder::MemoryMapForwarder(): impl_(new Impl()) {}
MemoryMapForwarder::~MemoryMapForwarder() { delete impl_; }

MemoryMapForwarder & MemoryMapForwarder::instance() {
    static MemoryMapForwarder s_instance;
    return s_instance;
}

void MemoryMapForwarder::record_allocation(VkDevice device, VkDeviceMemory memory, VkDeviceSize allocation_size) {
    impl_->record_allocation(device, memory, allocation_size);
}

void MemoryMapForwarder::forget_allocation(VkDeviceMemory memory) { impl_->forget_allocation(memory); }

VkDeviceSize MemoryMapForwarder::test_get_allocation_size(VkDeviceMemory memory) const { return impl_->test_get_allocation_size(memory); }

VkResult MemoryMapForwarder::custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                                      void ** ppData) {
    return impl_->custom_vkMapMemory_entry(device, memory, offset, size, flags, ppData);
}

void MemoryMapForwarder::custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory) { impl_->custom_vkUnmapMemory_entry(device, memory); }

// ---- MemoryMapReceiver::Impl ------------------------------------------------

class MemoryMapReceiver::Impl {
public:
    bool custom_vkMapMemory_endpoint(const CommandStream & /*request_stream*/, const Range & /*request_range*/, CommandStream & /*response_stream*/) {
        // Placeholder: no custom command unpack, driver call, mapping record, or
        // response written yet. Standard generated vkMapMemory must not depend on
        // this path; it exists for vkfwd's custom memory-map command id.
        VKFWD_LOG_ERROR("vkfwd MemoryMapReceiver: custom_vkMapMemory_endpoint not yet implemented (placeholder)");
        return false;
    }

    bool custom_vkUnmapMemory_endpoint(const CommandStream & /*request_stream*/, const Range & /*request_range*/, CommandStream & /*response_stream*/) {
        // Placeholder: no custom command unpack, staged-byte copy, cache
        // maintenance, or driver unmap call yet.
        return false;
    }
};

// ---- MemoryMapReceiver ------------------------------------------------------

MemoryMapReceiver::MemoryMapReceiver(): impl_(new Impl()) {}
MemoryMapReceiver::~MemoryMapReceiver() { delete impl_; }

bool MemoryMapReceiver::custom_vkMapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream) {
    return impl_->custom_vkMapMemory_endpoint(request_stream, request_range, response_stream);
}

bool MemoryMapReceiver::custom_vkUnmapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream) {
    return impl_->custom_vkUnmapMemory_endpoint(request_stream, request_range, response_stream);
}

} // namespace vkfwd
