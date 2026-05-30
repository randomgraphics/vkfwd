#include "trace_replay_executor.hpp"

#include <cstdio>

namespace vkfwd {

void TraceReplayExecutor::replay(const InterceptedCall & call) {
    // This is a receiver-side placeholder, not successful Vulkan replay. Keeping
    // the wording explicit helps logs distinguish capture from restored execution.
    std::fprintf(stderr, "vkfwd receiver: replay placeholder %.*s\n", static_cast<int>(call.name.size()), call.name.data());
}

} // namespace vkfwd
