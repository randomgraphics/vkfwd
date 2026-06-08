#pragma once

#include "command_stream.hpp"
#include "custom_command.hpp"

namespace vkfwd::receiver {
struct ReplayContext;
}

namespace vkfwd::receiver {

// Routes vkfwd-owned manual command ids (the [kReservedCommandIdBase, 2^32)
// range) to the matching handler. Returns true if the manual id was recognized
// and the handler succeeded; false otherwise (the receiver session aborts the
// stream on false to surface protocol errors loudly).
bool dispatch_manual_command(::vkfwd::manual::CommandId command_id, const CommandStream & request_stream, const Range & request_range,
                             CommandStream & response_stream, ReplayContext & replay_context);

} // namespace vkfwd::receiver
