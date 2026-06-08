#pragma once

#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 1 fills these methods in with real receiver
// staging behavior. Until then every method returns false so the receiver
// endpoint reports a replay failure rather than silently doing nothing.
class NonCoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
};

} // namespace vkfwd::memory_map
