#include "null_api_endpoint.hpp"

#include <cstdio>

namespace vkfwd {

void NullApiEndpoint::call(const SerializedCall& call) {
  // TextCallSerializer currently guarantees a trailing NUL. Real endpoints must
  // not rely on that convention; they should treat SerializedCall as opaque
  // bytes and produce the Vulkan-visible outputs required by the command.
  const auto* name = reinterpret_cast<const char*>(call.bytes.data());
  std::fprintf(stderr, "vkfwd: serialized call %s\n", name);
}

} // namespace vkfwd
