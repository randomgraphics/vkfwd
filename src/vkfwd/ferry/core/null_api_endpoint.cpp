#include "null_api_endpoint.hpp"

#include <cstdio>

namespace vkfwd {

Blob NullApiEndpoint::flush(Blob& request_blob) {
  // This endpoint is a trace-only placeholder. A real endpoint owns the
  // transport/replay details below this boundary and must produce the
  // Vulkan-visible response blob for the last command in the flushed stream.
  std::fprintf(stderr, "vkfwd: flushed %zu request bytes\n", request_blob.size());
  return Blob{};
}

} // namespace vkfwd
