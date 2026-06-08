#pragma once

#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder; phase 3 (or whichever phase ships the coherent
// strategy) fills the methods. Until then every method returns false.
class CoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) override;
};

} // namespace vkfwd::memory_map
