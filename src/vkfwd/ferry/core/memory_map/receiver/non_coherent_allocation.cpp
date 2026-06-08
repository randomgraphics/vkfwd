#include "memory_map/receiver/non_coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

bool NonCoherentReceiverAllocation::map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR("vkfwd: NonCoherentReceiverAllocation::map_endpoint not yet implemented (phase 0)");
    return false;
}

bool NonCoherentReceiverAllocation::unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) { return false; }

bool NonCoherentReceiverAllocation::flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) { return false; }

bool NonCoherentReceiverAllocation::invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

} // namespace vkfwd::memory_map
