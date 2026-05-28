#pragma once

#include "call_record.hpp"

namespace vkfwd {

class TraceReplayExecutor final : public ReplayExecutor {
public:
  // Replay tracing is intentionally separate from forwarding. It gives tests a
  // receiver target before handle restoration and real vk* invocation exist.
  void replay(const InterceptedCall& call) override;
};

} // namespace vkfwd
