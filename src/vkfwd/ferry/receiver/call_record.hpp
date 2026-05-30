#pragma once

#include <string_view>

namespace vkfwd {

// Vulkan replay is fundamentally directional: most captured traffic flows from
// the application host to the receiver, but some future protocol messages will
// need to carry receiver-created results, failures, or synchronization feedback
// back to the host. Keeping direction in the receiver record model prevents the
// endpoint boundary from becoming host-only by accident.
enum class CallDirection {
    HostToReceiver,
    ReceiverToHost,
};

// This is intentionally only metadata in the scaffold. The eventual generated
// interceptors should replace this with call-specific records that own deep
// copies of every pointer, array, pNext chain, and memory payload needed for
// replay. The invariant is that by the time a call reaches receiver replay, it
// no longer depends on application-owned memory staying alive.
struct InterceptedCall {
    std::string_view name;
    CallDirection    direction = CallDirection::HostToReceiver;
};

class ReplayExecutor {
public:
    virtual ~ReplayExecutor() = default;

    // Replay is the point where source-side handles and receiver-side handles
    // must be reconciled. The executor contract is intentionally separate from
    // endpoint transport so tests can exercise ordering without requiring a
    // Vulkan device, and replay backends can own Vulkan dispatch state directly.
    virtual void replay(const InterceptedCall & call) = 0;
};

} // namespace vkfwd
