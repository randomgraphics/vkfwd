#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::memory_map {

MemoryTypeRegistry & MemoryTypeRegistry::instance() {
    static MemoryTypeRegistry s_instance;
    return s_instance;
}

void MemoryTypeRegistry::record_device(VkDevice device, VkPhysicalDevice physical_device) {
    // Null handles cannot identify a real device; guard so a misuse from a
    // hook (e.g. failed vkCreateDevice that still ran our after_response hook)
    // does not poison the cache.
    if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    device_to_physical_[device] = physical_device;
}

void MemoryTypeRegistry::forget_device(VkDevice device) {
    std::lock_guard lock(mutex_);
    device_to_physical_.erase(device);
}

void MemoryTypeRegistry::record_memory_properties(VkPhysicalDevice physical_device, const VkPhysicalDeviceMemoryProperties & properties) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    memory_properties_[physical_device] = properties;
}

void MemoryTypeRegistry::record_non_coherent_atom_size(VkPhysicalDevice physical_device, VkDeviceSize size) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    non_coherent_atom_[physical_device] = size;
}

void MemoryTypeRegistry::record_min_memory_map_alignment(VkPhysicalDevice physical_device, std::size_t alignment) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    min_memory_map_alignment_[physical_device] = alignment;
}

std::optional<MemoryTypeRegistry::Resolved> MemoryTypeRegistry::resolve(VkDevice device, std::uint32_t memory_type_index) const {
    std::lock_guard lock(mutex_);

    const auto device_iter = device_to_physical_.find(device);
    if (device_iter == device_to_physical_.end()) { return std::nullopt; }
    const VkPhysicalDevice physical_device = device_iter->second;

    const auto props_iter = memory_properties_.find(physical_device);
    if (props_iter == memory_properties_.end()) { return std::nullopt; }
    const auto & props = props_iter->second;
    if (memory_type_index >= props.memoryTypeCount) { return std::nullopt; }

    const auto atom_iter = non_coherent_atom_.find(physical_device);
    if (atom_iter == non_coherent_atom_.end()) { return std::nullopt; }

    const auto map_alignment_iter = min_memory_map_alignment_.find(physical_device);
    if (map_alignment_iter == min_memory_map_alignment_.end()) { return std::nullopt; }

    return Resolved {
        .property_flags           = props.memoryTypes[memory_type_index].propertyFlags,
        .non_coherent_atom_size   = atom_iter->second,
        .min_memory_map_alignment = map_alignment_iter->second,
    };
}

} // namespace vkfwd::memory_map
