#pragma once

#include "forwarding_sink.hpp"

namespace vkfwd {

class NullForwardingSink final : public ForwardingSink {
public:
  // This sink is for bring-up and tests only. It proves that capture reached a
  // handoff boundary while making no promise about persistence, delivery, or
  // receiver-side execution.
  void submit(const SerializedCall& call) override;
};

} // namespace vkfwd
