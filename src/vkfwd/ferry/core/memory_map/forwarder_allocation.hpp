#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>

namespace vkfwd::memory_map {

// Per-VkDeviceMemory polymorphic handler. One concrete subclass is
// instantiated at vkAllocateMemory time and lives until vkFreeMemory.
// Subclasses implement the strategy-specific behavior (non-coherent vs
// coherent); the manager holds them by base pointer.
class ForwarderAllocation {
public:
    struct CreationInfo {
        VkDevice              device;
        VkDeviceMemory        memory;
        VkDeviceSize          allocation_size;
        std::uint32_t         memory_type_index;
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        // Required to satisfy Vulkan's *ppData - offset alignment contract
        // when phase 1 allocates source-side staging. Carried on every
        // allocation so subclasses do not have to re-resolve it via the
        // registry.
        std::size_t min_memory_map_alignment;
    };

    virtual ~ForwarderAllocation() = default;

    virtual VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) = 0;
    virtual void     unmap()                                                                            = 0;
    virtual VkResult flush(VkDeviceSize offset, VkDeviceSize size)                                      = 0;
    virtual VkResult invalidate(VkDeviceSize offset, VkDeviceSize size)                                 = 0;

    const CreationInfo & info() const { return info_; }

protected:
    explicit ForwarderAllocation(const CreationInfo & info): info_(info) {}

private:
    CreationInfo info_;
};

} // namespace vkfwd::memory_map
