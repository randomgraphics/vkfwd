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
    // transport session. Each request stream starts with a fixed RequestStreamHeader,
    // so Forwarder does not allocate per-thread transport objects.
    static void set_transport_creator(TransportCreator creator);

    CommandStream & request_stream() { return request_stream_; }
    void            reset_request_stream();

    /// Flush the accumulated request stream to the transport session, then reset
    /// it with the same source-thread header. Returns the receiver response stream.
    CommandStream flush();

    /// The forwared function pointer querier
    ///@{
    static PFN_vkVoidFunction VKAPI_CALL getInstanceProcAddr(VkInstance instance, const char * name);
    static PFN_vkVoidFunction VKAPI_CALL getDeviceProcAddr(VkDevice device, const char * name);
    ///@}

private:
    Forwarder();

    // The Forwarder itself is thread-local, so this stream is already per-thread
    // state. Forwarder owns the request framing invariant: every accumulated
    // request stream starts with a RequestStreamHeader carrying this id.
    StreamId      stream_id_ = 0;
    CommandStream request_stream_;

    std::shared_ptr<TransportSession> transport_;
};

} // namespace vkfwd
