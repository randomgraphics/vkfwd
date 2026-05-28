#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vkfwd {

// Vulkan replay is fundamentally directional: most captured traffic flows from
// the application host to the receiver, but some future protocol messages will
// need to carry receiver-created results, failures, or synchronization feedback
// back to the host. Keeping direction in the shared record model prevents the
// serializer boundary from becoming host-only by accident.
enum class CallDirection {
  HostToReceiver,
  ReceiverToHost,
};

// This is intentionally only metadata in the scaffold. The eventual generated
// interceptors should replace this with call-specific records that own deep
// copies of every pointer, array, pNext chain, and memory payload needed for
// replay. The invariant is that by the time a call reaches serialization, it no
// longer depends on application-owned memory staying alive.
struct InterceptedCall {
  std::string_view name;
  CallDirection direction = CallDirection::HostToReceiver;
};

// SerializedCall owns its bytes because forwarding may be asynchronous. A sink
// must be able to queue, batch, or move the payload without retaining references
// into the layer's stack frame or into the original application's parameters.
struct SerializedCall {
  std::vector<std::uint8_t> bytes;
};

class CallSerializer {
public:
  virtual ~CallSerializer() = default;

  // Implementations are responsible for producing a replay-stable byte stream,
  // not just a trace. For real Vulkan calls that means preserving enough shape
  // information to rebuild optional pointers, counted arrays, chained structs,
  // handles, and result-bearing output parameters on the receiver side.
  virtual SerializedCall serialize(const InterceptedCall& call) = 0;
};

class CallDeserializer {
public:
  virtual ~CallDeserializer() = default;

  // The returned record may borrow from the input span only if the caller
  // consumes it synchronously before the bytes are released. Long-lived replay
  // queues should deserialize into owning, call-specific records instead.
  virtual InterceptedCall deserialize(std::span<const std::uint8_t> bytes) = 0;
};

class ReplayExecutor {
public:
  virtual ~ReplayExecutor() = default;

  // Replay is the point where source-side handles and receiver-side handles
  // must be reconciled. The executor contract is intentionally separate from
  // deserialization so tests can validate the wire format without requiring a
  // Vulkan device, and replay backends can own Vulkan dispatch state directly.
  virtual void replay(const InterceptedCall& call) = 0;
};

} // namespace vkfwd
