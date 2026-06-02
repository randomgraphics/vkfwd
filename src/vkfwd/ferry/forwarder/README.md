# vkfwd ferry forwarder

`forwarder` is the source-process Vulkan layer. It exposes generated Vulkan
entry points to the loader, packs source API calls into core command blobs, and
sends flushed streams through a shared `TransportSession`.

## Responsibilities

- `layer.cpp`: exported `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`
  implementation for the Vulkan loader.
- `forwarder.hpp` and `forwarder.cpp`: thread-local request stream and
  transport-session ownership.
- `../core/generated/dispatch_table.*`: generated function-pointer table types
  and name-lookup methods for commands that vkfwd currently supports. The table
  code intentionally does not declare API entry points so table shape stays
  separate from wrapper linkage.
- `generated/forwarder_entrypoints.*`: generated declarations and populated
  dispatch-table instances for the Vulkan layer entry point wrappers. Wrapper
  symbols use a `_entry` suffix so they cannot collide with loader/exported
  Vulkan names.
- `generated/entry/*_entry.cpp`: generated Vulkan layer entry-point functions
  stored in the generated dispatch tables and called by application code through
  the Vulkan loader.
- `test/`: handwritten in-process tests for selected forwarder entry-point
  behavior at a test transport boundary.
- `manifest/`: Vulkan layer manifest template.

## Loader and Dispatch Invariants

The forwarder exposes only vkfwd-owned generated entry points. Unknown commands
return null from `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` until vkfwd owns
their generated pack, response, and output-parameter contract.

`vkGetInstanceProcAddr(nullptr, name)` only exposes loader-global commands owned
by vkfwd. With a non-null instance, `vkGetInstanceProcAddr(instance, name)` may
also return vkfwd-owned instance and device command trampolines; Vulkan allows
applications to acquire device command entry points this way before a device is
created. `vkGetDeviceProcAddr` stays device-scoped and only exposes generated
device commands for non-null devices.

The generated dispatch tables follow the Vulkan object lifecycle:

- global: initialized before a `VkInstance` exists and holding
  `vkGetInstanceProcAddr` plus loader-global commands such as
  `vkEnumerateInstanceVersion`, `vkEnumerateInstanceLayerProperties`,
  `vkEnumerateInstanceExtensionProperties`, and `vkCreateInstance`
- instance: initialized after `vkCreateInstance` succeeds and holding
  `vkGetDeviceProcAddr` plus instance-level generated entry points
- device: initialized after `vkCreateDevice` succeeds and holding device-level
  generated entry points

These tables point to vkfwd wrappers, never to the local Vulkan driver or a
downstream loader-chain dispatch table. The current forwarder does not call a
local driver and does not maintain per-instance or per-device dispatch chains.
Receiver-side replay is responsible for destination dispatch.

## Forwarder State

`Forwarder::instance()` is thread-local. Each thread owns:

- one request `CommandStream`
- one stable 64-bit source-thread token embedded at the start of each request
  stream
- one shared `TransportSession` created from the process-wide transport creator

Configure the transport creator before application worker threads enter Vulkan.
Concrete transports may multiplex internally, but `Forwarder` only depends on
the synchronous `send_accumulated_api_calls()` boundary.

## Generated Entry-Point Flow

Response-bearing commands follow this shape:

1. run an optional manual pre-pack hook
2. copy function arguments into generated `Command::Parameters`
3. append a command chunk to `Forwarder::request_stream()` after the source-thread
   prefix
4. call `Forwarder::flush()`, which sends the thread's stream through the transport
   session and resets it with the same prefix
5. unpack the returned response stream
6. copy response-owned output parameter values back to the caller
7. run an optional manual post-response hook
8. return the response return value

Deferrable commands currently have no return value and no output parameters.
They only append their command chunk to the thread-local request stream. A later
response-bearing command or an explicit test flush sends the pending stream.
`vkDestroyInstance` is intentionally not deferred even though it has no response:
the generated wrapper flushes it as a lifecycle fence so receiver-side replay can
drain deferred commands before destroying the destination instance.

## Transport Boundary

`TransportSession::send_accumulated_api_calls()` receives a stream whose first 64
bits are the source-thread token and whose remaining bytes may contain multiple
command chunks. The transport owns framing, remote or local delivery, replay
coordination, response correlation, and handle mapping below this boundary. The
generated forwarder wrapper only knows how to decode the response stream for the
last response-bearing command in the flushed stream.

Do not add replay behavior, local Vulkan dispatch, or source-to-destination
handle maps to this module. Put those policies in concrete transport/receiver
code.

## Generated Code and Hooks

Files under `forwarder/generated/` are generated. Update
`src/vkfwd/ferry/script/generator/vulkan_metadata.py` and regenerate instead of
editing them directly.

Manual forwarder hooks may live under a future `forwarder/hook/` tree and are
conditionally included by generated wrappers when present. Hook code should stay
command-specific and document why it is needed.

## Testing Guidance

Handwritten forwarder tests install a test transport session, call selected
generated Vulkan entry points, validate the forwarding boundary, and return a
generated response stream when the command requires one. These tests protect
entry-point behavior such as flushing, response propagation, and output-value
copy-back; exhaustive command and structure pack/unpack coverage belongs in the
generated core round-trip tests.

When adding a supported command, update the generator so it emits:

- the core command pack/unpack model
- the forwarder entry point
- any structure pack/unpack support it needs

Add or update a handwritten forwarder entry-point test only when the command
introduces new forwarding behavior that is not already covered by the existing
selected entry-point tests.
