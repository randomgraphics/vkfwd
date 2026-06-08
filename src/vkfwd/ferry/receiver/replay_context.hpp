#pragma once

#include "generated/dispatch_table.hpp"
#include "memory_map_manager.hpp"

namespace vkfwd::receiver {

struct ReplayContext {
    // Receiver replay needs a mutable context instead of a bare const dispatch
    // table because successful create/destroy commands change what can be
    // replayed next. For now only dispatch is modeled; source-to-destination
    // handle maps, unpack scratch storage, scheduling state, and replay errors
    // belong here as endpoint replay grows beyond direct function invocation.
    ::vkfwd::generated::DistributionTable dispatch {};

    // Per-context memory map manager: owns receiver-side mapped ranges and the
    // staging-byte transfer protocol for vkMapMemory / vkUnmapMemory.
    ::vkfwd::MemoryMapReceiver memoryMap;
};

} // namespace vkfwd::receiver
