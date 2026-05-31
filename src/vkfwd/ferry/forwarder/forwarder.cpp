#include "forwarder.hpp"

#include "null_transport_channel.hpp"

#include <memory>

namespace vkfwd {
namespace {

std::unique_ptr<TransportChannel> make_null_channel() { return std::make_unique<NullTransportChannel>(); }

Forwarder::ChannelCreator & channel_creator_slot() {
    // This process-level creator is configuration, not the hot forwarding state.
    // It should be set during layer/test startup before application threads begin
    // calling Vulkan entry points.
    static Forwarder::ChannelCreator creator = make_null_channel;
    return creator;
}

} // namespace

Forwarder & Forwarder::instance() {
    thread_local Forwarder forwarder;
    return forwarder;
}

void Forwarder::set_channel_creator(ChannelCreator creator) { channel_creator_slot() = creator ? creator : make_null_channel; }

Forwarder::Forwarder(): channel_(channel_creator_slot()()) {
    if (!channel_) { channel_ = make_null_channel(); }
}

Blob Forwarder::flush() {
    Blob response_blob = channel().send(request_blob_);
    request_blob_.reset();
    return response_blob;
}

TransportChannel & Forwarder::channel() { return *channel_; }

} // namespace vkfwd
