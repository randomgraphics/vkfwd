#pragma once

#include "call_record.hpp"

namespace vkfwd {

class ForwardingSink {
public:
  virtual ~ForwardingSink() = default;

  // The sink is a narrow handoff boundary by design. IPC, sockets, capture
  // files, and in-process tests should all consume the same serialized call
  // object so transport decisions do not leak into Vulkan parameter capture.
  virtual void submit(const SerializedCall& call) = 0;
};

} // namespace vkfwd
