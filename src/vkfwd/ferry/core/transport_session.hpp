#pragma once

#include "protocol.hpp"
#include "transport_channel.hpp"

#include <memory>

namespace vkfwd {

struct TransportSessionInfo {
    HandshakeRequest local_handshake;
    HandshakeRequest remote_handshake;
};

class TransportSession {
public:
    virtual ~TransportSession() = default;

    // The session owns compatibility negotiation before any channel carries
    // command bytes. Keeping the handshake at session scope lets many thread-local
    // channels share one remote-device connection without repeating schema checks
    // on every Vulkan call.
    virtual const TransportSessionInfo & info() const = 0;

    // Forwarder-side entry point: allocate a dedicated logical channel for one
    // source thread. A concrete channel may retain a shared_ptr/reference back to
    // this session for multiplexing, flow control, and response demultiplexing.
    virtual std::unique_ptr<TransportChannel> open_channel() = 0;

    // Receiver-side entry point: wait for or poll the next logical channel opened
    // by the peer. The receiver then attaches replay state to that channel without
    // leaking transport details into Vulkan replay code.
    virtual std::unique_ptr<TransportChannel> accept_channel() = 0;
};

} // namespace vkfwd
