#pragma once

#include "api_endpoint.hpp"
#include "call_record.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <mutex>
#include <string_view>

namespace vkfwd {

class Forwarder {
public:
  static Forwarder& instance();

  // This setter exists so tests and future runtime configuration can swap the
  // API endpoint without changing the Vulkan layer entry points. The layer
  // should stay focused on interception mechanics.
  void set_endpoint(std::unique_ptr<ApiEndpoint> endpoint);

  // capture() is the layer-side boundary: callers provide a fully captured
  // Vulkan call record, and this object dispatches it to the configured
  // endpoint. Today the record is minimal; the invariant for the real
  // implementation is that all borrowed Vulkan parameter memory has already
  // been converted into replayable owned data before the endpoint receives it.
  void capture(const InterceptedCall& call);

  template <class ParameterPacket, class ResponsePacket>
  VkResult forward(std::string_view name,
                   const ParameterPacket& parameter_packet,
                   const ResponsePacket& placeholder_response,
                   ResponsePacket* response_packet) {
    // The generated forwarder path is intentionally pack-first: any pointer,
    // array, or pNext lifetime work must happen before endpoint submission.
    // This scaffold still submits only the command name, so it returns a
    // generated response placeholder until ApiEndpoint grows a real
    // command-response payload carrying return values and output parameters.
    (void)parameter_packet;
    if (!response_packet) {
      return VK_ERROR_UNKNOWN;
    }
    capture({name});
    *response_packet = placeholder_response;
    return VK_SUCCESS;
  }

private:
  Forwarder() = default;

  std::mutex mutex_;
  std::unique_ptr<ApiEndpoint> endpoint_;
};

} // namespace vkfwd
