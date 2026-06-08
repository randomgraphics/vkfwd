#include "memory_map/manager.hpp"

#include "logging.hpp"
#include "memory_map/forwarder_allocation.hpp"
#include "memory_map/forwarder_allocation_factory.hpp"
#include "memory_map/receiver_allocation.hpp"
#include "memory_map/receiver_allocation_factory.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace vkfwd {

// ---- MemoryMapForwarder::Impl -----------------------------------------------

class MemoryMapForwarder::Impl {
public:
    void record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment) {
        if (memory == VK_NULL_HANDLE) { return; }

        auto allocation = ::vkfwd::memory_map::ForwarderAllocationFactory::create({
            .device                   = device,
            .memory                   = memory,
            .allocation_size          = allocation_size,
            .memory_type_index        = memory_type_index,
            .property_flags           = property_flags,
            .non_coherent_atom_size   = non_coherent_atom_size,
            .min_memory_map_alignment = min_memory_map_alignment,
        });
        // Non-host-visible allocations have no map-manager identity. The
        // factory returns nullptr; record nothing and let any later map call
        // on this handle fail at the lookup below.
        if (!allocation) { return; }

        std::lock_guard lock(mutex);
        allocations[memory] = std::move(allocation);
    }

    void forget_allocation(VkDeviceMemory memory) {
        if (memory == VK_NULL_HANDLE) { return; }

        std::lock_guard lock(mutex);
        // unique_ptr destructor releases any staging owned by the subclass.
        allocations.erase(memory);
    }

    VkResult custom_vkMapMemory_entry(VkDevice /*device*/, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                      void ** ppData) {
        ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
        {
            std::lock_guard lock(mutex);
            const auto      found = allocations.find(memory);
            if (found == allocations.end()) {
                // Either the allocation was non-host-visible (factory skipped it)
                // or vkfwd could not classify it from cache. Either way, return a
                // visible error so callers can react without dereferencing a
                // stale receiver-side mapped pointer.
                if (ppData) { *ppData = nullptr; }
                VKFWD_LOG_ERROR("vkfwd: MemoryMapForwarder::custom_vkMapMemory_entry called for an unrecorded VkDeviceMemory");
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            allocation = found->second.get();
        }
        return allocation->map(offset, size, flags, ppData);
    }

    void custom_vkUnmapMemory_entry(VkDevice /*device*/, VkDeviceMemory memory) {
        ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
        {
            std::lock_guard lock(mutex);
            const auto      found = allocations.find(memory);
            if (found == allocations.end()) { return; }
            allocation = found->second.get();
        }
        allocation->unmap();
    }

    VkResult flush_ranges(VkDevice /*device*/, std::uint32_t range_count, const VkMappedMemoryRange * ranges) {
        if (range_count == 0 || ranges == nullptr) { return VK_SUCCESS; }
        VkResult aggregated = VK_SUCCESS;
        // Attempt every range so a single unknown handle does not skip valid
        // ranges later in the array. Surface only the first failure.
        for (std::uint32_t i = 0; i < range_count; ++i) {
            const auto &                               range      = ranges[i];
            ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
            {
                std::lock_guard lock(mutex);
                const auto      found = allocations.find(range.memory);
                if (found == allocations.end()) {
                    if (aggregated == VK_SUCCESS) { aggregated = VK_ERROR_FEATURE_NOT_PRESENT; }
                    continue;
                }
                allocation = found->second.get();
            }
            const VkResult r = allocation->flush(range.offset, range.size);
            if (r != VK_SUCCESS && aggregated == VK_SUCCESS) { aggregated = r; }
        }
        return aggregated;
    }

    VkResult invalidate_ranges(VkDevice /*device*/, std::uint32_t range_count, const VkMappedMemoryRange * ranges) {
        if (range_count == 0 || ranges == nullptr) { return VK_SUCCESS; }
        VkResult aggregated = VK_SUCCESS;
        for (std::uint32_t i = 0; i < range_count; ++i) {
            const auto &                               range      = ranges[i];
            ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
            {
                std::lock_guard lock(mutex);
                const auto      found = allocations.find(range.memory);
                if (found == allocations.end()) {
                    if (aggregated == VK_SUCCESS) { aggregated = VK_ERROR_FEATURE_NOT_PRESENT; }
                    continue;
                }
                allocation = found->second.get();
            }
            const VkResult r = allocation->invalidate(range.offset, range.size);
            if (r != VK_SUCCESS && aggregated == VK_SUCCESS) { aggregated = r; }
        }
        return aggregated;
    }

    VkDeviceSize test_get_allocation_size(VkDeviceMemory memory) const {
        std::lock_guard lock(mutex);
        const auto      found = allocations.find(memory);
        if (found == allocations.end()) { return 0; }
        return found->second->info().allocation_size;
    }

    mutable std::mutex                                                                            mutex;
    std::unordered_map<VkDeviceMemory, std::unique_ptr<::vkfwd::memory_map::ForwarderAllocation>> allocations;
};

// ---- MemoryMapForwarder -----------------------------------------------------

MemoryMapForwarder::MemoryMapForwarder(): impl_(new Impl()) {}
MemoryMapForwarder::~MemoryMapForwarder() { delete impl_; }

MemoryMapForwarder & MemoryMapForwarder::instance() {
    static MemoryMapForwarder s_instance;
    return s_instance;
}

void MemoryMapForwarder::record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                                           VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment) {
    impl_->record_allocation(device, memory, property_flags, memory_type_index, allocation_size, non_coherent_atom_size, min_memory_map_alignment);
}

void MemoryMapForwarder::forget_allocation(VkDeviceMemory memory) { impl_->forget_allocation(memory); }

VkResult MemoryMapForwarder::custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                                      void ** ppData) {
    return impl_->custom_vkMapMemory_entry(device, memory, offset, size, flags, ppData);
}

void MemoryMapForwarder::custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory) { impl_->custom_vkUnmapMemory_entry(device, memory); }

VkResult MemoryMapForwarder::flush_ranges(VkDevice device, std::uint32_t range_count, const VkMappedMemoryRange * ranges) {
    return impl_->flush_ranges(device, range_count, ranges);
}

VkResult MemoryMapForwarder::invalidate_ranges(VkDevice device, std::uint32_t range_count, const VkMappedMemoryRange * ranges) {
    return impl_->invalidate_ranges(device, range_count, ranges);
}

VkDeviceSize MemoryMapForwarder::test_get_allocation_size(VkDeviceMemory memory) const { return impl_->test_get_allocation_size(memory); }

// ---- MemoryMapReceiver::Impl ------------------------------------------------

class MemoryMapReceiver::Impl {
public:
    void record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment) {
        if (memory == VK_NULL_HANDLE) { return; }
        auto allocation = ::vkfwd::memory_map::ReceiverAllocationFactory::create({
            .device                   = device,
            .memory                   = memory,
            .allocation_size          = allocation_size,
            .memory_type_index        = memory_type_index,
            .property_flags           = property_flags,
            .non_coherent_atom_size   = non_coherent_atom_size,
            .min_memory_map_alignment = min_memory_map_alignment,
        });
        // Non-host-visible allocations have no receiver-side map identity;
        // factory returns nullptr. No mutex: the receiver is per-ReplayContext
        // and externally serialized by its owning source-thread stream.
        if (!allocation) { return; }
        allocations[memory] = std::move(allocation);
    }

    void forget_allocation(VkDeviceMemory memory) {
        if (memory == VK_NULL_HANDLE) { return; }
        allocations.erase(memory);
    }

    bool custom_vkMapMemory_endpoint(const CommandStream & /*request_stream*/, const Range & /*request_range*/, CommandStream & /*response_stream*/) {
        // Phase-0 placeholder. Phase 1 unpacks manual::CommandId::MemoryMap
        // and delegates into the matching ReceiverAllocation subclass.
        VKFWD_LOG_ERROR("vkfwd MemoryMapReceiver: custom_vkMapMemory_endpoint not yet implemented (placeholder)");
        return false;
    }

    bool custom_vkUnmapMemory_endpoint(const CommandStream & /*request_stream*/, const Range & /*request_range*/, CommandStream & /*response_stream*/) {
        return false;
    }

    std::unordered_map<VkDeviceMemory, std::unique_ptr<::vkfwd::memory_map::ReceiverAllocation>> allocations;
};

// ---- MemoryMapReceiver ------------------------------------------------------

MemoryMapReceiver::MemoryMapReceiver(): impl_(new Impl()) {}
MemoryMapReceiver::~MemoryMapReceiver() { delete impl_; }

void MemoryMapReceiver::record_allocation(VkDevice device, VkDeviceMemory memory, VkMemoryPropertyFlags property_flags, std::uint32_t memory_type_index,
                                          VkDeviceSize allocation_size, VkDeviceSize non_coherent_atom_size, std::size_t min_memory_map_alignment) {
    impl_->record_allocation(device, memory, property_flags, memory_type_index, allocation_size, non_coherent_atom_size, min_memory_map_alignment);
}

void MemoryMapReceiver::forget_allocation(VkDeviceMemory memory) { impl_->forget_allocation(memory); }

bool MemoryMapReceiver::custom_vkMapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream) {
    return impl_->custom_vkMapMemory_endpoint(request_stream, request_range, response_stream);
}

bool MemoryMapReceiver::custom_vkUnmapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream) {
    return impl_->custom_vkUnmapMemory_endpoint(request_stream, request_range, response_stream);
}

} // namespace vkfwd
