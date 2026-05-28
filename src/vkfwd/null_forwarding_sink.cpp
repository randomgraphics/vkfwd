#include "null_forwarding_sink.hpp"

#include <cstdio>

namespace vkfwd {

void NullForwardingSink::submit(const SerializedCall& call) {
  // TextCallSerializer currently guarantees a trailing NUL. Real sinks must not
  // rely on that convention; they should treat SerializedCall as opaque bytes.
  const auto* name = reinterpret_cast<const char*>(call.bytes.data());
  std::fprintf(stderr, "vkfwd: serialized call %s\n", name);
}

} // namespace vkfwd
