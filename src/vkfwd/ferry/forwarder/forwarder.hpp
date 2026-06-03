#pragma once

#include "command_stream.hpp"
#include "transport_session.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace vkfwd {

class Forwarder {
public:
    /// Return a thread-local forwarder instance.
    static Forwarder & instance();

    using TransportCreator = std::function<std::shared_ptr<TransportSession>()>;

    // Configure this before worker threads enter Vulkan. Each thread-local
    // Forwarder calls the creator from its constructor and reuses the process
    // transport session. Each request stream starts with a fixed StreamHeader,
    // so Forwarder does not allocate per-thread transport objects.
    static void set_transport_creator(TransportCreator creator);

    CommandStream & request_stream() {
        if (request_stream_.size() == 0) { request_stream_.reset(stream_id_); }
        return request_stream_;
    }

    /// Flush the accumulated request stream to the transport session, then reset
    /// it with the same source-thread header. Returns the receiver response stream.
    CommandStream flush();

private:
    Forwarder();

    // The Forwarder itself is thread-local, so this stream is already per-thread
    // state. Deferrable commands append here until a synchronous command flushes
    // it through the shared transport session.
    CommandStream request_stream_;

    std::shared_ptr<TransportSession> transport_;

    StreamId stream_id_ = 0;
};

} // namespace vkfwd
