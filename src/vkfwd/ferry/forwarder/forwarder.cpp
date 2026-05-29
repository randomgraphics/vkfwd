#include "forwarder.hpp"

#include "null_api_endpoint.hpp"

#include <memory>

namespace vkfwd {

Forwarder& Forwarder::instance() {
  static Forwarder forwarder;
  return forwarder;
}

void Forwarder::set_endpoint(std::unique_ptr<ApiEndpoint> endpoint) {
  std::lock_guard lock(mutex_);
  endpoint_ = std::move(endpoint);
}

void Forwarder::capture(const InterceptedCall& call) {
  std::lock_guard lock(mutex_);
  // Defaults keep the layer useful before configuration exists. They are
  // deliberately trace-only so a developer cannot mistake this path for remote
  // execution or complete Vulkan replay.
  if (!endpoint_) {
    endpoint_ = std::make_unique<NullApiEndpoint>();
  }

  endpoint_->call(call);
}

} // namespace vkfwd
