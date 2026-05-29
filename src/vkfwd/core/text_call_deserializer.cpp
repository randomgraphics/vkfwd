#include "text_call_deserializer.hpp"

namespace vkfwd {

InterceptedCall TextCallDeserializer::deserialize(
    std::span<const std::uint8_t> bytes) {
  // The text scaffold expects a NUL-terminated function name and returns a view
  // into the caller-owned bytes. Receiver::receive consumes it synchronously;
  // this must not be reused for queued or asynchronous replay.
  const auto* name = reinterpret_cast<const char*>(bytes.data());
  return InterceptedCall{name};
}

} // namespace vkfwd
