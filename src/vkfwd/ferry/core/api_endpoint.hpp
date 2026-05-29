#pragma once

#include "call_record.hpp"

namespace vkfwd {

class ApiEndpoint {
public:
  virtual ~ApiEndpoint() = default;

  // The endpoint is the top-level boundary for a captured API call. Real
  // implementations must complete enough local or remote execution to provide
  // the same caller-visible contract as a Vulkan driver call: return value,
  // output parameters, handle identities, ordering, and error behavior. Logging,
  // files, IPC, or network transport are implementation details below this
  // boundary, not the contract exposed to interceptors.
  virtual void call(const SerializedCall& call) = 0;
};

} // namespace vkfwd
