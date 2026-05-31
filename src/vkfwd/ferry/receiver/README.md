# vkfwd ferry receiver

The receiver is the destination-side runtime for forwarded Vulkan calls. It owns
per-channel request handling, source-to-receiver handle mapping, and replay
against the local Vulkan implementation.

Core transport concepts such as connection ownership, session handshake, channel
establishment, framing, and response correlation are documented in
`../core/README.md`. Receiver code should consume those contracts rather than
defining transport policy locally.

## Receiver Replay Boundary

Transport delivers bytes and preserves channel semantics. It must not own Vulkan
replay decisions. Receiver replay needs a separate backend that can own:

- Destination Vulkan dispatch tables.
- Source-to-receiver handle maps.
- Command-specific unpacked records with owned pointer, array, and `pNext` data.
- Replay ordering and synchronization policy.
- Response payload construction.

The current `Receiver::receive()` and `ReplayExecutor` are placeholders for this
boundary. A real receiver run loop should decode transport frames into
command-specific records, then submit those records to a replay scheduler.

## Endpoint and Generated Adapter Shape

`endpoint` is the better name for the receiver-side request/response boundary.
An endpoint receives decoded command intent, performs test or replay behavior,
and produces the response data that will be packed back to the forwarder.
`executor` should be reserved for a lower-level replay implementation that
actually invokes destination Vulkan commands after decoding, ordering, and handle
mapping have already been handled.

The receiver still needs generated per-API adapter code because unpacking
parameters, constructing typed responses, and packing responses are API-specific.
That generated adapter should reuse `core/generated/command/<api>` pack/unpack
functions; it must not duplicate serialization logic.

Avoid replacing an 800-method endpoint interface with duplicated 800-case
switches in every endpoint implementation. A better shape is:

- One generated command-id dispatch table for the supported API surface.
- One generated typed adapter per API. The adapter unpacks request parameters,
  calls the endpoint's typed operation, and packs any response.
- Endpoint implementations register or expose handler tables for only the APIs
  they support.
- Test endpoints can install a small table for a narrow command subset.
- A real Vulkan replay endpoint may eventually have many API-specific handlers,
  but it should not duplicate blob dispatch, unpack, or response packing logic.

For the current four-command slice, a generated receiver adapter might have
handlers for `vkCreateInstance`, `vkDestroyInstance`, `vkCreateDevice`, and
`vkDestroyDevice`. A test endpoint can provide four typed operations that compare
parameters and synthesize responses. A real replay endpoint can provide the same
four typed operations and call destination Vulkan after resolving source handles
and rehydrating any encoded pointer fields.

This design keeps the generated sets purposeful:

- Core generated code defines wire types and pack/unpack functions.
- Forwarder generated code implements source-side Vulkan ABI wrappers.
- Receiver generated code adapts command blobs to endpoint operations.
- Endpoint/replay implementations own behavior, handle maps, synchronization,
  and response values.

## Vulkan Ordering Constraints

Multi-channel transport is necessary for source-side multithreaded Vulkan, but
it is not enough by itself. The receiver must not blindly replay every channel in
parallel.

The replay scheduler must preserve:

- FIFO ordering within each source thread/channel.
- Visibility of handles created on one channel before use on another channel.
- Correct lifetime ordering for instance, device, queue, command pool, command
  buffer, memory, image, buffer, and synchronization objects.
- Vulkan externally synchronized object rules.
- Host-side synchronization implied by blocking commands and explicit barriers.

The conservative first implementation can serialize all replay globally after
the transport layer has demultiplexed channels. That leaves performance on the
table, but it is easier to validate. Later scheduling can relax global
serialization by locking or ordering around Vulkan objects that are externally
synchronized.

## Handle Mapping

Receiver-side Vulkan handles cannot be returned directly as source-side handles.
Dispatchable handles such as `VkInstance`, `VkDevice`, `VkQueue`, and
`VkCommandBuffer` are especially sensitive because loader dispatch depends on
the source process's handle representation.

The receiver must maintain a bidirectional mapping:

```text
source handle/token <-> receiver native Vulkan handle
```

Responses should return source-visible handle tokens or wrapper-compatible
values, not raw receiver pointers. Any command that creates or destroys handles
must update the map in an order visible to all channels that may reference those
handles.

## Open Implementation Questions

- Which concrete backend is first: local IPC, TCP, QUIC, or in-memory test
  transport?
- How are receiver addresses configured: environment variable, config file,
  command line, or layer setting?
- What architecture compatibility is supported initially? Same endian and
  pointer width should be assumed unless the blob schema is made portable.
- Which commands define the first real replay scope beyond create/destroy
  instance and device?
- Does the first receiver scheduler serialize globally, or does it implement
  object-aware locking from the start?
