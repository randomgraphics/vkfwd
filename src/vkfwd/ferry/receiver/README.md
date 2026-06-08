# vkfwd ferry receiver

The receiver is the destination-side runtime for forwarded Vulkan calls. It owns
per-source-thread request handling, source-to-receiver handle mapping, and replay
against the local Vulkan implementation.

Core transport concepts such as connection ownership, framing,
source-thread-token routing, session compatibility, and response correlation are
documented in `../core/README.md`. Receiver code should consume those contracts
rather than defining transport policy locally.

## Receiver Replay Boundary

Transport delivers bytes and preserves source-thread stream semantics. It must
not own Vulkan replay decisions. Receiver replay needs a separate backend that
can own:

- Destination Vulkan dispatch tables.
- Source-to-receiver handle maps.
- Command-specific unpacked records with owned pointer, array, and `pNext` data.
- Replay ordering and synchronization policy.
- Response payload construction.

`Receiver` is intentionally thin: it is constructed with a `ReceiverSession` and
a `ReplayContext`, then registers an API-responder factory with the session. The
session owns source-thread demultiplexing and responder lifetime. The concrete
responder validates accumulated request blobs, decodes command chunk headers,
resolves each generated command id through the replay context's distribution
table, and returns the response stream for the flushed stream. The replay context
is deliberately mutable because create/destroy commands update destination
dispatch tables (and will later update source-to-receiver handle maps, scratch
allocations, and replay error state as replay coverage grows).

Command-specific behavior lives in generated per-API endpoints (see below), which
own typed parameter unpacking, Vulkan invocation, and response payload packing.
Source-to-receiver handle mapping is not yet implemented; the current endpoints
replay against handles as decoded (the in-process loopback tests use synthetic
handles), and the Handle Mapping section below remains the target shape.

## Endpoint and Generated Adapter Shape

A receiver *endpoint* is the per-API function that decodes a forwarded command,
invokes the destination Vulkan call, and packs the response. Endpoints reuse the
`core/generated/command/<api>` pack/unpack helpers and must not duplicate
serialization logic.

The generated layout is:

- `generated/endpoints.{hpp,cpp}` — `call_api_endpoint(CommandId, ...)`, a single
  `switch` over `CommandId` that forwards to the per-group endpoint functions.
  This dispatcher does not grow in shape as API coverage expands.
- `generated/endpoint/<group>.{hpp,cpp}` — endpoint implementations split by
  API domain (e.g. `global_instance`, `physical_device`, `device_lifecycle`,
  `memory_buffer`). Group assignment is the `COMMAND_GROUP` manifest in
  `script/generator/vulkan_metadata.py`; the split keeps any one source file
  within the ferry-wide grouping limit documented in `../README.md`. The build
  globs `generated/endpoint/*.cpp`, so new groups need no `CMakeLists.txt` edit.

Each generated endpoint follows one uniform path: resolve the destination
function from the distribution table, unpack parameters, call Vulkan, then pack
the response. There is no 800-case switch and no per-endpoint duplication of
dispatch, unpack, or packing logic.

### Per-API hook framework

Per-API customization is opt-in via human-written hook headers, mirroring the
forwarder's `forwarder/hook/<api>ForwarderHook.hpp` mechanism. Authors specialize
`vkfwd::receiver::manual::CommandHooks<CommandId>` (base template in
`generated/receiver_hooks.hpp`) in `receiver/hook/<api>ReceiverHook.hpp`; the
generated endpoint conditionally calls each phase via
`if constexpr (Hooks::<phase>_enabled)`, so absent hooks cost nothing. The phases
are `before_unpack`, `before_call`, `after_call`, `before_pack_response`,
`after_pack_response`, and `replace_endpoint` (a full override used when the
endpoint must not run its standard generated body — for example, the
fail-closed `vkMapMemory` / `vkUnmapMemory` hooks below, whose supported
path is the manual memory-map command ids rather than these generated
chunks). The
generator owns no per-API customization data; it emits only the uniform endpoint
plus the opt-in hook call sites.

Worked examples: `vkCreateInstanceReceiverHook.hpp` / `vkCreateDeviceReceiverHook.hpp`
use `after_call` to initialize the receiver-owned instance/device dispatch tables
after a successful create; `vkMapMemoryReceiverHook.hpp` /
`vkUnmapMemoryReceiverHook.hpp` use `replace_endpoint` to fail closed on the
standard generated `vkMapMemory` / `vkUnmapMemory` chunks. The supported
memory-map path is the manual `vkfwd::manual::CommandId::MemoryMap` /
`MemoryUnmap` protocol owned by `MemoryMapReceiver`, not the generated
endpoints; the design rationale lives in `doc/memory_map_management.md`.

This keeps the generated sets purposeful:

- Core generated code defines wire types and pack/unpack functions.
- Forwarder generated code implements source-side Vulkan ABI wrappers.
- Receiver generated code adapts command blobs to endpoint operations.
- Hook headers and replay state own per-API behavior, handle maps,
  synchronization, and response values.

## Vulkan Ordering Constraints

Multi-thread transport is necessary for source-side multithreaded Vulkan, but it
is not enough by itself. The receiver must not blindly replay every source-thread
stream in parallel.

The replay scheduler must preserve:

- FIFO ordering within each source thread.
- Visibility of handles created by one source thread before use by another
  source thread.
- Correct lifetime ordering for instance, device, queue, command pool, command
  buffer, memory, image, buffer, and synchronization objects.
- Vulkan externally synchronized object rules.
- Host-side synchronization implied by blocking commands and explicit barriers.

The conservative first implementation can serialize all replay globally after
the transport layer has demultiplexed source-thread streams. That leaves
performance on the table, but it is easier to validate. Later scheduling can relax global
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
must update the map in an order visible to all source threads that may reference
those handles.

## Open Implementation Questions

- Which concrete backend is first: local IPC, TCP, QUIC, or in-memory test
  transport?
- How are receiver addresses configured: environment variable, config file,
  command line, or layer setting?
- What architecture compatibility is supported initially? Same endian and
  pointer width should be assumed unless the stream schema is made portable.
- Which commands define the first real replay scope beyond create/destroy
  instance and device?
- Does the first receiver scheduler serialize globally, or does it implement
  object-aware locking from the start?
