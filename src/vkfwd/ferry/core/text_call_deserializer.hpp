#pragma once

#include "call_record.hpp"

namespace vkfwd {

class TextCallDeserializer final : public CallDeserializer {
public:
  // Paired with TextCallSerializer for scaffold tests. Real deserialization
  // must validate call ids, payload sizes, pointer presence, and every counted
  // array before replay is allowed to touch Vulkan.
  InterceptedCall deserialize(std::span<const std::uint8_t> bytes) override;
};

} // namespace vkfwd
