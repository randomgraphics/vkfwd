#include "memory_map/receiver_allocation_factory.hpp"

#include "memory_map/receiver/coherent_allocation.hpp"
#include "memory_map/receiver/non_coherent_allocation.hpp"

namespace vkfwd::memory_map {

std::unique_ptr<ReceiverAllocation> ReceiverAllocationFactory::create(const ReceiverAllocation::CreationInfo & info) {
    const bool host_visible  = (info.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool host_coherent = (info.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!host_visible) { return nullptr; }
    if (host_coherent) { return std::make_unique<CoherentReceiverAllocation>(info); }
    return std::make_unique<NonCoherentReceiverAllocation>(info);
}

} // namespace vkfwd::memory_map
