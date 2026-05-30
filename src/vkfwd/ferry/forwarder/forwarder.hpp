#pragma once

#include "api_endpoint.hpp"
#include "blob.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace vkfwd {

class Forwarder {
public:
    using EndpointCreator = std::unique_ptr<ApiEndpoint> (*)();

    static Forwarder & instance();

    // Configure this before worker threads enter Vulkan. Each thread-local
    // Forwarder calls the creator from its constructor and owns the endpoint it
    // receives; any cross-thread transport sharing belongs inside endpoint
    // implementations, not in Forwarder.
    static void set_endpoint_creator(EndpointCreator creator);

    Blob & request_blob() { return request_blob_; }
    Blob   flush();

private:
    Forwarder();
    ApiEndpoint & endpoint();

    // The Forwarder itself is thread-local, so this blob is already per-thread
    // state. Deferrable commands append here until a synchronous command flushes
    // it through endpoint().
    Blob                         request_blob_;
    std::unique_ptr<ApiEndpoint> endpoint_;
};

} // namespace vkfwd
