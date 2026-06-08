#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace vkfwd::memory_map {

// Forwarder-side cache populated by hooks on vkCreateDevice,
// vkDestroyDevice, vkGetPhysicalDeviceMemoryProperties, and
// vkGetPhysicalDeviceProperties. Allocate-time code calls resolve() to
// translate (device, memoryTypeIndex) into the property flags and
// nonCoherentAtomSize / minMemoryMapAlignment it needs to construct a
// ForwarderAllocation with a source-visible staging pointer that preserves
// Vulkan's mapped-pointer alignment contract.
class MemoryTypeRegistry {
public:
    static MemoryTypeRegistry & instance();

    void record_device(VkDevice device, VkPhysicalDevice physical_device);
    void forget_device(VkDevice device);

    void record_memory_properties(VkPhysicalDevice physical_device,
                                  const VkPhysicalDeviceMemoryProperties & properties);
    void record_non_coherent_atom_size(VkPhysicalDevice physical_device,
                                       VkDeviceSize size);
    void record_min_memory_map_alignment(VkPhysicalDevice physical_device,
                                         std::size_t alignment);

    struct Resolved {
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        std::size_t           min_memory_map_alignment;
    };

    // Returns nullopt when the device, the physical device, or the memory
    // type index has not been populated yet. Callers must treat this as
    // "vkfwd cannot classify this allocation from cache yet", not as an
    // application Vulkan-order violation.
    std::optional<Resolved> resolve(VkDevice device,
                                    std::uint32_t memory_type_index) const;

private:
    MemoryTypeRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<VkDevice, VkPhysicalDevice>                         device_to_physical_;
    std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> memory_properties_;
    std::unordered_map<VkPhysicalDevice, VkDeviceSize>                     non_coherent_atom_;
    std::unordered_map<VkPhysicalDevice, std::size_t>                      min_memory_map_alignment_;
};

} // namespace vkfwd::memory_map
