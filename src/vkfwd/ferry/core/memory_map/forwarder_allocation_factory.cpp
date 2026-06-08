#include "memory_map/forwarder_allocation_factory.hpp"

#include "memory_map/forwarder/coherent_allocation.hpp"
#include "memory_map/forwarder/non_coherent_allocation.hpp"

namespace vkfwd::memory_map {

std::unique_ptr<ForwarderAllocation> ForwarderAllocationFactory::create(const ForwarderAllocation::CreationInfo & info) {
    const bool host_visible  = (info.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool host_coherent = (info.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    // Non-mappable allocations have no map-manager identity; the manager
    // skips recording. vkMapMemory on such a handle later fails at the
    // manager lookup; the receiver-side driver would reject it anyway.
    if (!host_visible) { return nullptr; }

    if (host_coherent) { return std::make_unique<CoherentForwarderAllocation>(info); }
    return std::make_unique<NonCoherentForwarderAllocation>(info);
}

} // namespace vkfwd::memory_map
