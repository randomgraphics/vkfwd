#include "text_call_serializer.hpp"

namespace vkfwd {

SerializedCall TextCallSerializer::serialize(const InterceptedCall& call) {
  SerializedCall serialized;
  // The NUL terminator is a scaffold convenience for trace sinks and the paired
  // text deserializer. It is not a wire-format decision for real Vulkan replay.
  serialized.bytes.assign(call.name.begin(), call.name.end());
  serialized.bytes.push_back(0);
  return serialized;
}

} // namespace vkfwd
