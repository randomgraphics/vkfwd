#pragma once

#include "api_endpoint.hpp"

namespace vkfwd {

class NullApiEndpoint final : public ApiEndpoint {
public:
    // This endpoint is for bring-up and tests only. It proves that bytes reached
    // the endpoint boundary, but it deliberately does not satisfy the real
    // response, handle mapping, or receiver-side Vulkan execution contract.
    Blob flush(Blob & request_blob) override;
};

} // namespace vkfwd
