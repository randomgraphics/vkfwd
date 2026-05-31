#pragma once

#include "transport_channel.hpp"

namespace vkfwd {

class NullTransportChannel final : public TransportChannel {
public:
    // This channel is for bring-up and tests only. It proves that bytes reached
    // the transport boundary, but it deliberately does not satisfy the real
    // forwarding contract because it never contacts a receiver or replays Vulkan.
    Blob send(Blob & request_blob) override;
};

} // namespace vkfwd
