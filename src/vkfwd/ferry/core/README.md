# vkfwd ferry core

`core` is the shared forwarding substrate. It is linked by both the Vulkan
forwarder layer and receiver-side replay code, so anything here defines behavior
on both sides of the forwarding boundary.

## Responsibilities

- Protocol metadata in `protocol.hpp`: stream magic, schema version, Vulkan API
  version negotiation, command chunk headers, and command chunk ranges.
- Blob storage in `blob.hpp` and `blob.cpp`: grow-only copied payload storage
  with stable logical offsets and bounded views.
- Transport contracts in `transport_channel.hpp` and `transport_session.hpp`:
  session-scoped handshake and per-thread logical channels for command streams.
- Generated command pack/unpack code in `generated/command/`.
- Generated structure pack/unpack code in `generated/structure/`.
- Manual command hooks in `hook/`.
- Small placeholder/debug implementations such as `NullTransportChannel`.

## Serialization Model

Command and structure serializers copy Vulkan inputs into a `Blob`. Pointers in
packed payloads are not source-process addresses after packing:

- Command parameter pointer slots use command-relative offsets where the command
  chunk is the base.
- Structure pointer slots use structure-relative offsets where the copied
  structure is the base.
- A null source pointer is encoded as a null pointer value.

Unpack functions currently validate the typed view and return a pointer into the
packed blob. They do not fully rehydrate pointer members into process-local
addresses. Replay code must resolve encoded offsets using the same base rule the
packer used.

## Blob Invariants

`Blob` owns copied bytes in stable chunks. Logical offsets are measured from the
beginning of the blob stream, not from a particular chunk allocation. Generated
code may store those offsets in pointer-typed Vulkan fields as an intermediate
wire representation.

When changing `Blob`, preserve these properties:

- `grow()` returns an aligned, bounded view over exactly the newly allocated
  range.
- `data_at()` returns a bounded view only when the entire requested range is
  present in one chunk.
- Failed or inconsistent count/pointer pairs must not expose out-of-bounds
  writable storage.

## pNext Policy

Generated structure code only packs known `pNext` node types. Unknown structures
are rejected instead of copied opaquely because they may contain source pointers,
callbacks, handles, or platform resources that are meaningless on the receiver.

The validator checks chains before copying nodes so a receiver does not observe a
partial chain. It must reject unsupported `sType` values, loops, excessive
depth, and unreadable/corrupt node memory without crashing on supported
platforms. Keep tests in `core/test/` aligned with every new validation rule.

For now, generated per-API tests intentionally use empty `pNext` chains unless a
test is specifically about structure `pNext` behavior.

## Transport Contracts

`TransportSession` owns compatibility negotiation. A session may open multiple
logical `TransportChannel`s, one per source thread. A channel `send()` call is
the synchronous forwarding boundary for a single thread's packed stream. The
request blob may contain zero or more deferrable commands followed by the command
that needs a response.

The transportation layer's goal is to carry already-packed vkfwd command bytes
from a source thread to a receiver replay context, then return the response blob
for the command that forced the flush. It should let the rest of ferry treat
local IPC, remote sockets, in-process tests, and future transports as the same
contract.

Required transportation-layer behavior:

- Negotiate `HandshakeRequest` compatibility once per `TransportSession` before
  any command bytes are exchanged.
- Provide one logical `TransportChannel` per source thread so deferrable command
  ordering remains per-thread and does not require locks in `Forwarder`.
- Preserve byte-for-byte blob contents and command-chunk order inside each
  channel send.
- Correlate every synchronous `send()` with exactly one returned response blob
  for the last response-bearing command in that flushed stream.
- Keep channel identity stable enough for receiver-side routing, logging, and
  future diagnostics.
- Own framing, multiplexing, flow control, retry/shutdown policy, and any
  transport-specific backpressure without leaking those details into generated
  command code.
- Define clear ownership of request and response blobs: callers retain the
  request blob object, while channel implementations may copy, move from, or
  synchronously inspect its bytes only within the documented `send()` contract.

The transportation layer must not reinterpret Vulkan command payloads beyond the
framing needed to route requests and responses. Vulkan replay ordering,
destination dispatch, synchronization, externally synchronized state, and
source-to-receiver handle mapping belong to receiver/replay code. Generated
packers own serialization; generated forwarder wrappers own caller-visible
return/output behavior.

Do not put Vulkan replay, destination dispatch, or handle mapping policy in
`Forwarder` or command packers. Those policies belong in concrete transport or
receiver code.

### Connection And Session Lifecycle

The default remote deployment should make the receiver listen and the forwarder
connect. A receiver process starts on the destination device, binds a configured
address, accepts a transport connection, completes the session handshake, and
then accepts logical channels.

For USB4 or Thunderbolt cable use, prefer OS-provided USB4/TB networking first,
then run the same process transport over that IP link. The core transport
contract must not depend on whether the physical link is USB4, Ethernet, local
IPC, or loopback. A later raw USB bulk backend can reuse the same session and
channel contracts if USB networking is not viable.

Expected backend progression:

- Local process-to-process IPC for tests and same-machine development.
- TCP or QUIC over USB4/TB networking for real cross-device communication.
- Raw USB bulk only if the device cannot expose a network interface.

Session creation sequence:

1. Receiver creates a listening transport backend.
2. Forwarder creates or reuses a process-wide remote session for the receiver
   address.
3. Both sides exchange `HandshakeRequest` data from `protocol.hpp`.
4. Receiver validates schema version, Vulkan major version, and Vulkan minor
   compatibility before accepting command traffic.
5. The session records local and remote handshake data in `TransportSessionInfo`.
6. Only after compatibility succeeds can the forwarder open channels.

Handshake is session-scoped, not channel-scoped. The intent is to let many
thread-local channels share one negotiated remote-device connection without
rechecking schema compatibility on every Vulkan call.

### Channel Lifecycle

Each source application thread owns a `thread_local Forwarder`, and each
`Forwarder` owns one `TransportChannel`. A concrete remote channel may hold a
reference to the shared `TransportSession`; `Forwarder` must not know about
session pooling, multiplexing, sockets, QUIC connections, or USB details.

Forwarder-side channel flow:

1. `Forwarder` is constructed on first Vulkan call from a source thread.
2. Its configured channel creator obtains a good shared session, creating and
   handshaking one if needed.
3. The channel creator calls `TransportSession::open_channel()`.
4. The returned channel becomes this thread's dedicated transport path.
5. `Forwarder::flush()` sends this thread's packed request blob through
   `TransportChannel::send()` and receives the response blob.

Receiver-side channel flow:

1. Receiver owns one accepted `TransportSession`.
2. Receiver waits on `TransportSession::accept_channel()`.
3. Each accepted channel gets a receiver-side channel context on demand.
4. The channel context owns per-channel request sequencing and replay state.
5. Requests are decoded into receiver-owned command records before replay.
6. Responses are sent back on the same logical channel and correlated with the
   originating request.

Per-channel order is FIFO. Different channels may be processed concurrently only
when Vulkan object synchronization and handle-map dependencies allow it.

### Framing And Correlation

Even when the backend is QUIC and already provides streams, vkfwd should keep an
explicit frame header. The header is the contract that makes IPC, TCP, QUIC, USB
bulk, and test transports interchangeable.

The frame metadata should include at least:

- Session or protocol magic.
- Schema or frame version.
- Channel id.
- Request sequence id.
- Message type: open, request, response, error, close, control.
- Flags: needs response, barrier, replay failure, transport failure.
- Payload byte size.

For `TransportChannel::send()`, the request sequence id is what lets the
forwarder block one source thread for its response while other channels continue
to make progress.

## Generated Core Code

`generated/command/`, `generated/structure/`, `generated/vulkan_api.hpp`, and
generated tests are produced by
`src/vkfwd/ferry/script/generator/vulkan_metadata.py`. Update the generator and
regenerate instead of editing these files directly.

Manual hook files under `hook/` may customize command behavior. Hook code must
document the command-specific invariant it is protecting, especially around
pointer ownership, lifetime, and source-to-receiver handle assumptions.

## Testing Guidance

- Put handwritten core tests under `core/test/` with an `internal-test.cmake`
  manifest.
- Keep generated structure tests under `core/generated/structure/test/`.
- Structure tests should validate both the top-level typed view and any copied
  pointer-owned payload such as strings, arrays, or nested structs.
- Negative serialization tests should assert failure codes and, where relevant,
  that output pointers are reset to null.
