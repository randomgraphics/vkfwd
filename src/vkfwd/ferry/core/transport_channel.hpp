#pragma once

#include "blob.hpp"

#include <cstdint>

namespace vkfwd {

using TransportChannelId = std::uint64_t;

constexpr TransportChannelId kInvalidTransportChannelId = 0;

class TransportChannel {
public:
    virtual ~TransportChannel() = default;

    virtual TransportChannelId id() const { return kInvalidTransportChannelId; }

    // send() is the forwarder-side synchronous boundary for one source thread's
    // packed Vulkan call stream. The request blob may contain deferred commands
    // followed by the command that requires a response; channel implementations
    // own transport framing, session routing, replay coordination, and response
    // correlation below this boundary.
    virtual Blob send(Blob & request_blob) = 0;

    // Receiver-side implementations will use the same channel identity, but the
    // receive/respond API is intentionally not exposed until the receiver run loop
    // owns command-specific request lifetimes and response correlation.
};

} // namespace vkfwd
