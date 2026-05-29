#pragma once

#include "api_endpoint.hpp"

namespace vkfwd {

class NullApiEndpoint final : public ApiEndpoint {
public:
  // This endpoint is for bring-up and tests only. It proves that capture reached
  // the top-level API boundary, but it deliberately does not satisfy the real
  // endpoint contract for output parameters, return values, handle mapping, or
  // receiver-side Vulkan execution.
  void call(const SerializedCall& call) override;
};

} // namespace vkfwd
