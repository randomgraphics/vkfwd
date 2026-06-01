#pragma once

#include "blob.hpp"
#include "protocol.hpp"

#include <memory>

namespace vkfwd {

struct TransportSessionInfo {
    HandshakeRequest local_handshake;
    HandshakeRequest remote_handshake;
};

class TransportSession {
public:
    virtual ~TransportSession() = default;

    // The session owns compatibility negotiation before any request stream carries
    // command bytes. Keeping the handshake at session scope lets many source
    // threads share one remote-device connection without repeating schema checks
    // on every Vulkan call.
    virtual const TransportSessionInfo & info() const = 0;

    // Forward one accumulated source-thread request stream and synchronously
    // return the response for the final response-bearing command in that stream.
    // The first 64 bits of every request stream are a vkfwd source-thread token;
    // transport implementations use that prefix for routing and correlation
    // instead of maintaining a separate per-thread transport object.
    virtual Blob send_accumulated_api_calls(Blob & request_blob) = 0;
};

} // namespace vkfwd
