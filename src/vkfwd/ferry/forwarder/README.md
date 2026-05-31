# vkfwd ferry forwarder

`forwarder` is the source-process Vulkan layer. It exposes generated Vulkan
entry points to the loader, packs source API calls into core command blobs, and
sends flushed streams through a per-thread `TransportChannel`.

## Responsibilities

- `layer.cpp`: exported `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`
  implementation for the Vulkan loader.
- `forwarder.hpp` and `forwarder.cpp`: thread-local request blob and
  transport-channel ownership.
- `generated/dispatch_table.*`: generated function-pointer tables for commands
  that vkfwd currently supports.
- `generated/command/*.cpp`: generated Vulkan entry-point wrappers.
- `generated/test/`: generated in-process tests that drive the forwarder entry
  points and validate the packed blobs at a test transport boundary.
- `manifest/`: Vulkan layer manifest template.

## Loader and Dispatch Invariants

The forwarder exposes only vkfwd-owned generated entry points. Unknown commands
return null from `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` until vkfwd owns
their generated pack, response, and output-parameter contract.

The generated dispatch tables are shared by command level:

- global: `vkCreateInstance`, `vkGetInstanceProcAddr`,
  `vkGetDeviceProcAddr`
- instance: instance-level generated entry points
- device: device-level generated entry points

These tables point to vkfwd wrappers, never to the local Vulkan driver or a
downstream loader-chain dispatch table. The current forwarder does not call a
local driver and does not maintain per-instance or per-device dispatch chains.
Receiver-side replay is responsible for destination dispatch.

## Forwarder State

`Forwarder::instance()` is thread-local. Each thread owns:

- one request `Blob`
- one `TransportChannel` created from the process-wide channel creator

Configure the channel creator before application worker threads enter Vulkan.
Concrete channels may share a session internally, but `Forwarder` only depends
on the per-thread `send()` boundary.

## Generated Entry-Point Flow

Response-bearing commands follow this shape:

1. run an optional manual pre-pack hook
2. copy function arguments into generated `Command::Parameters`
3. append a command chunk to `Forwarder::request_blob()`
4. call `Forwarder::flush()`, which sends the thread's blob through the channel
   and resets it
5. unpack the returned response blob
6. copy response-owned output parameter values back to the caller
7. run an optional manual post-response hook
8. return the response return value

Deferrable commands currently have no return value and no output parameters.
They only append their command chunk to the thread-local request blob. A later
response-bearing command or an explicit test flush sends the pending stream.

## Transport Boundary

`TransportChannel::send()` receives a blob that may contain multiple command
chunks. The channel owns framing, remote or local transport, replay
coordination, response correlation, and handle mapping below this boundary. The
generated forwarder wrapper only knows how to decode the response blob for the
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

Generated forwarder tests install a test transport channel, call the generated
Vulkan entry point, validate the received request blob inside the channel, and
return a generated response blob when the command requires one. This tests the
entry-point logic, not just command pack/unpack helpers.

When adding a supported command, update the generator so it emits:

- the core command pack/unpack model
- the forwarder entry point
- any structure pack/unpack support it needs
- a generated forwarder test covering input parameters, output parameters, and
  return value propagation
