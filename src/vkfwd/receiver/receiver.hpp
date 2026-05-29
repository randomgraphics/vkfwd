#pragma once

#include "call_record.hpp"

#include <memory>
#include <span>

namespace vkfwd {

class Receiver {
public:
  Receiver();

  // Receiver dependencies are injectable because replay needs two very
  // different test modes: byte-format validation without Vulkan, and real
  // Vulkan invocation with a receiver-owned dispatch table and handle map.
  void set_deserializer(std::unique_ptr<CallDeserializer> deserializer);
  void set_executor(std::unique_ptr<ReplayExecutor> executor);

  // receive() owns the receiver-side pipeline boundary. The input is one
  // complete serialized call, not an arbitrary stream fragment; framing belongs
  // to a future transport or capture-file layer.
  void receive(std::span<const std::uint8_t> bytes);

private:
  std::unique_ptr<CallDeserializer> deserializer_;
  std::unique_ptr<ReplayExecutor> executor_;
};

} // namespace vkfwd
