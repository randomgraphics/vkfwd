#include "receiver.hpp"

#include "text_call_deserializer.hpp"
#include "trace_replay_executor.hpp"

#include <memory>

namespace vkfwd {

Receiver::Receiver()
    // Defaults mirror the layer scaffold: they make the receiver usable for
    // smoke tests while making no claim that Vulkan state is reconstructed.
    : deserializer_(std::make_unique<TextCallDeserializer>()),
      executor_(std::make_unique<TraceReplayExecutor>()) {}

void Receiver::set_deserializer(std::unique_ptr<CallDeserializer> deserializer) {
  deserializer_ = std::move(deserializer);
}

void Receiver::set_executor(std::unique_ptr<ReplayExecutor> executor) {
  executor_ = std::move(executor);
}

void Receiver::receive(std::span<const std::uint8_t> bytes) {
  // The deserialized record is consumed immediately, which permits temporary
  // string_view-style records during bring-up. A queued receiver must switch to
  // owning records before storing work across receive() calls.
  executor_->replay(deserializer_->deserialize(bytes));
}

} // namespace vkfwd
