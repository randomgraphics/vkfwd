#pragma once

#include "blob.hpp"
#include "transport_channel.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace vkfwd {

class Forwarder {
public:
    using ChannelCreator = std::unique_ptr<TransportChannel> (*)();

    static Forwarder & instance();

    // Configure this before worker threads enter Vulkan. Each thread-local
    // Forwarder calls the creator from its constructor and owns the channel it
    // receives. A real channel may hold a reference to a shared transport session,
    // but Forwarder only relies on this per-thread send boundary.
    static void set_channel_creator(ChannelCreator creator);

    Blob & request_blob() { return request_blob_; }
    Blob   flush();

private:
    Forwarder();
    TransportChannel & channel();

    // The Forwarder itself is thread-local, so this blob is already per-thread
    // state. Deferrable commands append here until a synchronous command flushes
    // it through the thread's transport channel.
    Blob                              request_blob_;
    std::unique_ptr<TransportChannel> channel_;
};

} // namespace vkfwd
