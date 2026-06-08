#pragma once

#include "memory_map/forwarder_allocation.hpp"

#include <memory>

namespace vkfwd::memory_map {

class ForwarderAllocationFactory {
public:
    // Builds the concrete subclass that matches the memory type's property
    // flags. Returns nullptr for non-host-visible allocations — the manager
    // records nothing for them, so a later vkMapMemory on that handle fails
    // at the manager lookup rather than reaching this code.
    static std::unique_ptr<ForwarderAllocation> create(const ForwarderAllocation::CreationInfo & info);
};

} // namespace vkfwd::memory_map
