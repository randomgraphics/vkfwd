# Repository Structure

This document describes the high-level source layout for `vkfwd`.
The goal is to keep each implementation strategy self-contained so the project
can compare per-API-call forwarding, a stateful Vulkan front end, or a hybrid
without mixing their runtime assumptions.

## Implementation Families

`src/vkfwd` is split by implementation family:

- `ferry`: the current mechanical per-API-call forwarding path. It owns the
  shared command model, generated pack/unpack code, forwarder layer,
  receiver/replay scaffolding, generator scripts, and tests for that approach.
- `facade`: a placeholder for a future stateful Vulkan front end. It will own
  local Vulkan-facing state, local handle identities, and any forwarding/replay
  policy that depends on those local objects.

Code, scripts, generated files, and tests should stay inside the implementation
folder whose invariants they assume. Shared top-level code should be introduced
only when both families need the same contract and can preserve the same Vulkan
loader, dispatch, handle-mapping, and replay-ordering assumptions.

## Runtime Roles

`vkfwd` has two essential runtime roles:

- **Interceptor layer**: a Vulkan layer shared library loaded into the source
  application. It owns Vulkan loader entry points, dispatch-table chaining,
  source-side capture, and source-visible API completion.
- **Receiver executable**: a process on the destination machine or destination
  runtime. It accepts command payloads, unpacks them into receiver-owned data,
  replays them against the local Vulkan implementation, and sends required
  results back through the endpoint/transport path.

Optional tools should be modeled as variants of those roles rather than as new
core abstractions:

- **Recorder layer**: an interception-layer variant that calls through to the
  local driver while also writing replayable command payloads to a file.
- **Replay executable**: a tool that reads a saved command stream and either
  replays it locally or forwards it to a receiver endpoint.

## Main Components

The `ferry` core library should contain these major parts.

### Pack/Unpack

Pack/unpack is the center of the project. It owns the generated command data
model, binary payload schema, command ids, payload revisions, and the generated
code that converts Vulkan API parameters into replayable bytes and back into
receiver-owned records.

Command-specific pack/unpack code belongs in `core/generated/`, with
human-owned command customization in `core/hook/`. Keeping those pieces
together makes the generated payload schema, generated code, and manual hook
boundary visible in one place.

This component must not retain application-owned pointers in replayable
payloads. Pointer-bearing parameters, counted arrays, strings, `pNext` chains,
memory ranges, output values, and handle identities need explicit ownership and
compatibility rules.

### API Endpoint

The API endpoint is the end of an intercepted Vulkan call from the
application's point of view. Sending a serialized command into a complete
endpoint should behave like calling the real driver for the subset of Vulkan
that endpoint supports: it must return required values, populate output
parameters, preserve ordering, and maintain source-to-receiver handle identity.

Endpoint implementations can serve different purposes:

- Local debug execution that unpacks and immediately calls the local driver.
- Remote execution that sends payloads to a receiver and waits for required
  responses.
- Test doubles that validate pack/unpack behavior deterministically.
- Dumping or tracking endpoints that log calls, provided they are wrapped by an
  endpoint that still completes the Vulkan-visible API contract when required.

Transport, file writing, and logging are implementation details below this
boundary. A pure queue or file writer is not a complete endpoint unless it also
resolves the caller-visible API result.

### Transport

The transport layer moves framed bytes and responses between two endpoints. It
may be in-process, IPC, network, or file-backed depending on the endpoint/tool.
It should not know Vulkan command semantics beyond framing, ordering, session
metadata, and failure propagation needed by the endpoint contract.

### Common Utilities And State

Shared utilities include logging, diagnostics, protocol handshake code,
compatibility checks, command stream framing, and common bookkeeping types.
Receiver-only state such as destination Vulkan dispatch tables and handle maps
belongs in receiver/replay code, but the shared type definitions that describe
source identities and protocol contracts belong in core.

## Source Layout

The current source shape is:

```text
src/vkfwd/
  ferry/
    core/
      api_endpoint.*
      call_record.*
      protocol.*
      ...
      generated/
      hook/
    forwarder/
      forwarder.cpp
      ...
      generated/
      manifest/
    receiver/
      receiver.cpp
      ...
    scripts/
      generator/
    tests/

  facade/
    README.md
    scripts/
    tests/
```

Generated code should stay beside the implementation boundary it serves.
`ferry/core/generated/` remains generator-owned for pack/unpack data, payload
schemas, command ids, and shared Vulkan metadata for the per-call path.
`ferry/forwarder/generated/` remains generator-owned for forwarder-specific
layer entry points, dispatch lookup tables, and supported-API interceptor glue.
`ferry/core/hook/` remains human-owned for command-specific pack/unpack
customization. Generated command files and per-command schemas should stay close
to each command so review and compatibility decisions do not collapse into one
large central file.

Other ferry core files should stay flat directly under `ferry/core/` for now.
Add more subfolders only when an implementation boundary becomes large enough
that the extra directory earns its keep.

## CMake Target Shape

The build expresses these runtime boundaries:

- `vkfwd_core`: static library containing generated pack/unpack code, endpoint
  interfaces, transport interfaces, hooks, protocol, and utilities for `ferry`.
- `vkfwd_forwarder`: shared library loaded by the Vulkan loader; links
  `vkfwd_core` and owns `ferry` forwarder-specific generated interception code.
- `vkfwd_receiver`: receiver library that hosts the receiver endpoint and replay
  scaffolding; links `vkfwd_core`. It can grow an executable entry point when
  process/runtime policy exists.
- `vkfwd_recorder`: optional shared library variant; links `vkfwd_core`.
- `vkfwd_replay`: optional executable for saved streams; links `vkfwd_core`.

The forwarder target should remain a thin Vulkan loader/dispatch adapter.
Receiver targets should own replay dispatch, destination Vulkan state, and
handle mapping. The core library should define contracts and shared payload
handling, but it should not accidentally become a hidden global runtime.

## Remaining Growth Points

1. Add generated forwarding dispatch/interceptor glue under
   `src/vkfwd/ferry/forwarder/generated/` when the forwarding layer generator is
   introduced.
2. Add receiver executable entry points only after receiver process/runtime
   policy is defined.
3. Add recorder and replay-tool folders when their endpoint behavior is defined
   enough that they cannot be confused with trace-only placeholders.

During that growth, comments should keep calling out placeholder behavior so
trace-only paths are not mistaken for complete forwarding or replay.
