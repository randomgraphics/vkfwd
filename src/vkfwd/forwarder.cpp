#include "forwarder.hpp"

#include "null_forwarding_sink.hpp"
#include "text_call_serializer.hpp"

#include <cstdio>
#include <memory>

namespace vkfwd {

Forwarder& Forwarder::instance() {
  static Forwarder forwarder;
  return forwarder;
}

void Forwarder::set_serializer(std::unique_ptr<CallSerializer> serializer) {
  std::lock_guard lock(mutex_);
  serializer_ = std::move(serializer);
}

void Forwarder::set_sink(std::unique_ptr<ForwardingSink> sink) {
  std::lock_guard lock(mutex_);
  sink_ = std::move(sink);
}

void Forwarder::capture(const InterceptedCall& call) {
  std::lock_guard lock(mutex_);
  // Defaults keep the layer useful before configuration exists. They are
  // deliberately trace-only so a developer cannot mistake this path for remote
  // execution or complete Vulkan replay.
  if (!serializer_) {
    serializer_ = std::make_unique<TextCallSerializer>();
  }
  if (!sink_) {
    sink_ = std::make_unique<NullForwardingSink>();
  }

  sink_->submit(serializer_->serialize(call));
}

} // namespace vkfwd
