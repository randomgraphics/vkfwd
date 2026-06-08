#pragma once

#include "memory_map/receiver_allocation.hpp"

#include <memory>

namespace vkfwd::memory_map {

class ReceiverAllocationFactory {
public:
    // Phase 0 has no callers; the branching is identical to the forwarder
    // factory. Phase 1 wires this into the receiver-side state used by
    // manual::CommandId::MemoryMap / MemoryUnmap.
    static std::unique_ptr<ReceiverAllocation> create(const ReceiverAllocation::CreationInfo & info);
};

} // namespace vkfwd::memory_map
