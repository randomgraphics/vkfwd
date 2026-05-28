#pragma once

#include "call_record.hpp"

namespace vkfwd {

class TextCallSerializer final : public CallSerializer {
public:
  // Temporary serializer for validating the capture path. It intentionally
  // cannot represent Vulkan parameters; keeping it separate makes it easy to
  // replace with generated binary serialization without changing the layer.
  SerializedCall serialize(const InterceptedCall& call) override;
};

} // namespace vkfwd
