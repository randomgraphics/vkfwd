#include "memory_map/receiver/coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

bool CoherentReceiverAllocation::map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR("vkfwd: CoherentReceiverAllocation::map_endpoint not yet implemented (phase 0)");
    return false;
}

bool CoherentReceiverAllocation::unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) { return false; }

bool CoherentReceiverAllocation::flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) { return false; }

bool CoherentReceiverAllocation::invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

} // namespace vkfwd::memory_map
