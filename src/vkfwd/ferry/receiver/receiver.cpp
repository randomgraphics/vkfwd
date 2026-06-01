#include "receiver.hpp"

#include "trace_replay_executor.hpp"

#include <memory>

namespace vkfwd {

// The default executor is trace-only; it should not be confused with
// complete Vulkan replay or receiver-side handle restoration.
Receiver::Receiver(): executor_(std::make_unique<TraceReplayExecutor>()) {}

void Receiver::set_executor(std::unique_ptr<ReplayExecutor> executor) { executor_ = std::move(executor); }

void Receiver::receive(const InterceptedCall & call) {
    // The record is consumed immediately. A queued receiver must switch to owning
    // command-specific records before storing work across receive() calls.
    executor_->replay(call);
}

} // namespace vkfwd
