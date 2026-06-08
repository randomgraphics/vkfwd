# vkfwd ferry core

`core` is the shared forwarding substrate. It is linked by both the Vulkan
forwarder layer and receiver-side replay code, so anything here defines behavior
on both sides of the forwarding boundary.

## Responsibilities

- Command stream metadata and storage in `command_stream.hpp` and
  `command_stream.cpp`: stream magic, schema version, request/command chunk
  headers, command chunk ranges, and grow-only copied payload storage with
  stable logical offsets and bounded views.
- Generated API metadata in `generated/vulkan_api.hpp`: stable command ids and
  Vulkan API version types/values used by future session compatibility checks.
- Transport contracts in `transport_session.hpp` and `receiver_session.hpp`:
  session-scoped compatibility and source-thread-token routing for accumulated
  command streams.
- Generated command pack/unpack code in `generated/command/`.
- Generated dispatch-table types and command-name lookup helpers in
  `generated/dispatch_table.*`.
- Generated structure pack/unpack code in `generated/structure/`.
- Manual command hooks in `hook/`.
- Small placeholder/debug implementations used during forwarder bring-up.

## Serialization Model

Command and structure serializers copy Vulkan inputs into a `CommandStream`. Pointers in
packed payloads are not source-process addresses after packing:

- Every non-null pointer slot uses a field-relative byte offset: the encoded
  value is measured from the pointer field's own storage location to the copied
  target bytes.
- A null source pointer is encoded as a null pointer value.

Unpack functions take a mutable `SafeArrayView<std::uint8_t>` that starts at the
serialized command chunk or structure record. They validate that view, repair
encoded pointer fields in place by adding each offset to its own field address,
and return a pointer into the same stream-backed storage.

## CommandStream Invariants

`CommandStream` owns copied bytes in stable chunks. Logical offsets are measured from the
beginning of the stream stream, not from a particular chunk allocation. Generated
code may store those offsets in pointer-typed Vulkan fields as an intermediate
wire representation.

When changing `CommandStream`, preserve these properties:

- `grow()` returns an aligned, bounded view over exactly the newly allocated
  range; callers that need the logical CommandStream offset request it from `grow()`
  separately because `SafeArrayView` is only an accessor.
- `at()` returns a bounded view only when the entire requested range is
  present in one chunk.
- `is_contiguous()` reports whether the current logical stream is backed by a
  single allocation; use it for whole-stream inspection decisions, not allocation
  tuning.
- `flatten()` returns a new CommandStream whose logical stream is copied into one backing
  allocation so transports that need a contiguous byte span do not have to know
  about chunk internals.
- `SafeArrayView` callers access storage through `at()`; code that needs a
  typed pointer must first validate the view and then take the address of a
  checked element.
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

`TransportSession` owns compatibility negotiation and the synchronous forwarding
boundary. `TransportSession::send_accumulated_api_calls()` sends one source
thread's accumulated request stream and returns the response stream for the command
that forced the flush. The request stream begins with a fixed
`RequestStreamHeader` carrying the stream id, then contains
zero or more deferrable commands followed by the command that needs a response.

The transportation layer's goal is to carry already-packed vkfwd command bytes
from a source thread to a receiver replay context, then return the response stream
for the command that forced the flush. It should let the rest of ferry treat
local IPC, remote sockets, in-process tests, and future transports as the same
contract.

Required transportation-layer behavior:

- Preserve the leading `RequestStreamHeader` so deferrable command
  ordering remains per thread and does not require locks in `Forwarder`.
- Preserve byte-for-byte stream contents and command-chunk order inside each
  accumulated stream. Alignment padding between chunks is serialized as zero
  bytes and skipped by receiver-side chunk discovery; command ids must still be
  read from a valid `CommandChunkHeader`, not guessed from padding.
- Correlate every synchronous `send_accumulated_api_calls()` with exactly one
  returned response stream for the last response-bearing command in that flushed
  stream.
- Return response bytes in a form that generated forwarder code can inspect as
  one contiguous serialized command response. Test transports flatten receiver
  blobs at this boundary because receiver-side packing may use multiple arena
  chunks for large output arrays.
- Keep stream identity stable enough for receiver-side routing, logging,
  and future diagnostics.
- Own framing, multiplexing, flow control, retry/shutdown policy, and any
  transport-specific backpressure without leaking those details into generated
  command code.
- Define clear ownership of request and response blobs: callers retain the
  request stream object, while transport implementations may copy, move from, or
  synchronously inspect its bytes only within the documented
  `send_accumulated_api_calls()` contract.

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
address, accepts a transport connection, validates any required session
compatibility metadata, and then accepts accumulated request streams.

For USB4 or Thunderbolt cable use, prefer OS-provided USB4/TB networking first,
then run the same process transport over that IP link. The core transport
contract must not depend on whether the physical link is USB4, Ethernet, local
IPC, or loopback. A later raw USB bulk backend can reuse the same session and
stream contracts if USB networking is not viable.

Expected backend progression:

- Local process-to-process IPC for tests and same-machine development.
- TCP or QUIC over USB4/TB networking for real cross-device communication.
- Raw USB bulk only if the device cannot expose a network interface.

Session creation sequence:

1. Receiver creates a listening transport backend.
2. Forwarder creates or reuses a process-wide remote session for the receiver
   address.
3. Receiver validates any transport-level compatibility metadata before
   accepting command traffic.
4. Only after compatibility succeeds can the forwarder send accumulated request
   streams.

Compatibility is session-scoped. The intent is to let many thread-local
forwarders share one negotiated remote-device connection without rechecking
schema compatibility on every Vulkan call. The concrete negotiation messages are
deliberately left out until a transport backend needs them.

### Source-Thread Stream Lifecycle

Each source application thread owns a `thread_local Forwarder`, and each
`Forwarder` prefixes its request stream with one stable `RequestStreamHeader`.
`Forwarder` must not know about session pooling, multiplexing, sockets, QUIC
connections, or USB details.

Forwarder-side stream flow:

1. `Forwarder` is constructed on first Vulkan call from a source thread.
2. Its configured transport creator obtains a good shared session, creating and
   handshaking one if needed.
3. The forwarder constructs its request stream with the stream id already stored
   in the fixed request-stream header.
4. `Forwarder::flush()` sends this thread's packed request stream through
   `TransportSession::send_accumulated_api_calls()` and receives the response
   stream.

Receiver-side stream flow:

1. Receiver owns one accepted `TransportSession`.
2. Receiver reads each accumulated request stream from the transport.
3. `Receiver` registers an API-responder factory with `ReceiverSession`.
4. A concrete `ReceiverSession` reads the leading stream header and
   creates or reuses a responder for that token.
5. The responder owns per-source-thread request sequencing, generated command-id
   dispatch, and any implementation-owned replay state.
6. Responses are returned through the same transport session and correlated with
   the originating request.

Per-source-thread order is FIFO. Different source-thread streams may be processed
concurrently only when Vulkan object synchronization and handle-map dependencies
allow it.

### Framing And Correlation

Even when the backend is QUIC and already provides streams, vkfwd should keep an
explicit frame header. The header is the contract that makes IPC, TCP, QUIC, USB
bulk, and test transports interchangeable.

The frame metadata should include at least:

- Session or protocol magic.
- Schema or frame version.
- Stream id.
- Request sequence id.
- Message type: open, request, response, error, close, control.
- Flags: needs response, barrier, replay failure, transport failure.
- Payload byte size.

For `TransportSession::send_accumulated_api_calls()`, the stream id plus a
request sequence id is what lets the forwarder block one source stream for its
response while other source-thread streams continue to make progress.

## Generated Core Code

`generated/command/`, `generated/structure/`, `generated/vulkan_api.hpp`, and
generated tests are produced by
`src/vkfwd/ferry/script/generator/vulkan_metadata.py`. Update the generator and
regenerate instead of editing these files directly.

Generated command headers may stay per command so each Vulkan API keeps a stable
typed `Parameters` and `Response` contract. Generated command implementation
sources should be grouped by the ferry-wide API-domain policy in
`../README.md`, with normal groups around 25-60 APIs and a hard split before a
group grows past roughly 75 APIs. Do not add a new compiled core `.cpp` per
command as the default expansion path.

Generated structure serializers should follow the separate structure grouping
policy in `../README.md`. Structure groups are organized around nested pointer
graphs, pNext contracts, and Vulkan object domains rather than mirroring command
groups exactly. Shared pNext and common pointer-offset helpers belong in
`generated/structure/core` files; command create-info graphs should live in
domain structure groups such as pipeline, descriptor, image, sync, or
memory/buffer as the API surface expands. Every copied pointer, nested array,
and pNext handoff must have matching generated pack/unpack tests.

Manual hook files under `hook/` may customize command behavior. Hook code must
document the command-specific invariant it is protecting, especially around
pointer ownership, lifetime, and source-to-receiver handle assumptions.

Forwarder-side memory allocation bookkeeping is owned by
`MemoryMapForwarder`: successful `vkAllocateMemory` records the caller-visible
`VkDeviceMemory` allocation size, and accepted `vkFreeMemory` removes that
record. The registry exists only to resolve future source-side map ranges such
as `VK_WHOLE_SIZE`; it is not a receiver handle map and does not transfer Vulkan
memory ownership.

`vkMapMemory` and `vkUnmapMemory` are the exception to standard generated
command forwarding. The public Vulkan entry points delegate to
`MemoryMapForwarder::custom_vkMapMemory_entry` /
`custom_vkUnmapMemory_entry`, which use vkfwd-owned manual command ids instead
of generated Vulkan `CommandId::MapMemory` / `CommandId::UnmapMemory` payloads.
The receiver mapped pointer stays private to `MemoryMapReceiver`; only
source-owned staging pointers are returned to the application.

## Mapped Memory Correspondence

The mapped-memory design — why `vkMapMemory` cannot return the receiver's
mapped pointer, what state the forwarder/receiver each own, the chosen N2
non-coherent strategy and C2 coherent strategy (with their rejected
alternatives), the manual `vkfwd::manual::CommandId::MemoryMap` /
`MemoryUnmap` / `QueryPhysicalDeviceMemoryInfo` wire protocol, the
reserve+commit source-side staging layout, and the phase-by-phase landing
plan — is documented in **`doc/memory_map_management.md`**. Treat that
document as the single source of truth; do not duplicate decisions here.

Local invariants that affect any code in this directory:

- `vkMapMemory` and `vkUnmapMemory` are **not** forwarded via the standard
  generated `CommandId::MapMemory` / `CommandId::UnmapMemory` payloads.
  Public Vulkan entry points delegate to
  `MemoryMapForwarder::custom_vkMapMemory_entry` /
  `custom_vkUnmapMemory_entry`, which emit manual command ids. Receiver
  hooks for the standard generated map/unmap commands must fail closed.
- Receiver-side mapped pointers stay private to `MemoryMapReceiver` /
  `ReceiverAllocation`. They must never be serialized into a response or
  exposed on the manager's public surface.
- `MemoryMapForwarder`'s per-handle map is bookkeeping for range
  resolution (e.g., `VK_WHOLE_SIZE`); it is not a source-to-receiver
  handle map.
- The manual command id partition with generated ids is enforced
  structurally via `command_id_range.hpp::kReservedCommandIdBase` plus
  per-command `static_assert`s; do not bypass it by introducing untagged
  manual ids elsewhere.

Generated command packers intentionally drop `VkAllocationCallbacks`
parameters and encode those slots as null. Allocation callbacks contain
guest-process function pointers and user data, so they cannot be
marshalled into a receiver process; replay must use the receiver's
default allocator unless a future receiver-local allocator policy is
added.

## Testing Guidance

- Put handwritten core tests under `core/test/` with an `internal-test.cmake`
  manifest.
- Keep generated structure tests under `core/generated/structure/test/`.
- Keep generated command pack/flatten/unpack round-trip tests under
  `core/generated/command/test/`.
- Generate command and structure tests in the same domain groups as the generated
  implementation sources. Do not create a test source per API when grouped
  tests can provide the same dedicated `TEST_CASE` coverage.
- Structure tests should be grouped by structure serializer group, not by command
  group. A command may reference structures from several serializer groups; the
  structure tests should stay with the structure group that owns the pointer and
  pNext invariants.
- Structure tests should validate both the top-level typed view and any copied
  pointer-owned payload such as strings, arrays, or nested structs after
  flattening and unpacking. These tests protect the receiver-side invariant that
  every unpacked pointer resolves to locally valid storage in the flattened
  stream; command tests add the same coverage for command parameters, responses,
  and output handles passed to host Vulkan entry points.
- Negative serialization tests should assert failure codes and, where relevant,
  that output pointers are reset to null.
