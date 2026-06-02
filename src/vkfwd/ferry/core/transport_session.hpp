#pragma once

#include "command_stream.hpp"
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

    // Forward one accumulated source-thread request stream and synchronously
    // return the response for the final response-bearing command in that stream.
    // The first 64 bits of every request stream are a vkfwd source-thread token;
    // transport implementations use that prefix for routing and correlation
    // instead of maintaining a separate per-thread transport object.
    virtual CommandStream send_accumulated_api_calls(CommandStream & request_stream) = 0;
};

} // namespace vkfwd
