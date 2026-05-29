#include "null_api_endpoint.hpp"

#include <cstdio>

namespace vkfwd {

void NullApiEndpoint::call(const InterceptedCall& call) {
  // This endpoint is a trace-only placeholder. A real endpoint owns the
  // transport/replay details below this boundary and must produce the
  // Vulkan-visible outputs required by each forwarded command.
  std::fprintf(stderr, "vkfwd: captured call %.*s\n",
               static_cast<int>(call.name.size()),
               call.name.data());
}

} // namespace vkfwd
