#include "forwarder.hpp"

#include "null_api_endpoint.hpp"

#include <memory>

namespace vkfwd {
namespace {

std::unique_ptr<ApiEndpoint> make_null_endpoint() { return std::make_unique<NullApiEndpoint>(); }

Forwarder::EndpointCreator & endpoint_creator_slot() {
    // This process-level creator is configuration, not the hot forwarding state.
    // It should be set during layer/test startup before application threads begin
    // calling Vulkan entry points.
    static Forwarder::EndpointCreator creator = make_null_endpoint;
    return creator;
}

} // namespace

Forwarder & Forwarder::instance() {
    thread_local Forwarder forwarder;
    return forwarder;
}

void Forwarder::set_endpoint_creator(EndpointCreator creator) { endpoint_creator_slot() = creator ? creator : make_null_endpoint; }

Forwarder::Forwarder(): endpoint_(endpoint_creator_slot()()) {
    if (!endpoint_) { endpoint_ = make_null_endpoint(); }
}

Blob Forwarder::flush() {
    Blob response_blob = endpoint().flush(request_blob_);
    request_blob_.reset();
    return response_blob;
}

ApiEndpoint & Forwarder::endpoint() { return *endpoint_; }

} // namespace vkfwd
