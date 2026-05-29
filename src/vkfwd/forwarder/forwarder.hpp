#pragma once

#include "api_endpoint.hpp"
#include "call_record.hpp"

#include <memory>
#include <mutex>

namespace vkfwd {

class Forwarder {
public:
  static Forwarder& instance();

  // These setters exist so tests and future runtime configuration can swap the
  // serialization format and API endpoint without changing the Vulkan layer
  // entry points. The layer should stay focused on interception mechanics.
  void set_serializer(std::unique_ptr<CallSerializer> serializer);
  void set_endpoint(std::unique_ptr<ApiEndpoint> endpoint);

  // capture() is the layer-side boundary: callers provide a fully captured
  // Vulkan call record, and this object owns the policy of serialization plus
  // endpoint dispatch. Today the record is minimal; the invariant for the real
  // implementation is that all borrowed Vulkan parameter memory has already
  // been converted into replayable owned data, and the configured endpoint has
  // produced the caller-visible result required for the API, before this
  // function returns.
  void capture(const InterceptedCall& call);

private:
  Forwarder() = default;

  std::mutex mutex_;
  std::unique_ptr<CallSerializer> serializer_;
  std::unique_ptr<ApiEndpoint> endpoint_;
};

} // namespace vkfwd
