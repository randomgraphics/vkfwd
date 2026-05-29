# vkfwd

`vkfwd` is an experimental Vulkan API forwarder. The goal is to load a Vulkan
interception layer alongside a Vulkan application, capture the application's
Vulkan API traffic, and forward that workload to another process or a remote
machine for rendering.

The core problem is Vulkan API interception and faithful call reconstruction:
every intercepted call must have its parameters, pointed-to structures, chained
`pNext` data, handles, memory ranges, synchronization state, and ordering
serialized into a replayable stream. On the receiving side, that stream must be
deserialized and invoked against the real Vulkan implementation.

Forwarding to another process or remote machine is intentionally an internal
endpoint detail for now. The project starts with two main modules plus an
abstract API endpoint so the interception path can treat local replay, remote
replay, capture files, or test doubles as implementations of one driver-like
call boundary.

The intended long-term shape is:

- `vkfwd_forwarder`: a Vulkan explicit layer implementation that
  intercepts instance, device, queue, command buffer, memory, synchronization,
  and presentation calls, then serializes every parameter needed to replay them.
- `vkfwd_receiver`: a receiver library/runtime that deserializes the captured
  stream, reconstructs Vulkan object state, and invokes actual Vulkan API calls
  on the receiving side.
- An API endpoint interface that completes intercepted calls with the same
  caller-visible contract as a Vulkan driver call. Internally, an endpoint may
  use local IPC, remote network transports, capture files, or in-process test
  harnesses.

In this design, the endpoint is the end of an intercepted API call from the
application's point of view. A real endpoint may log, serialize, queue, or send
work elsewhere internally, but it must return the value, write output
parameters, and establish handle identities needed for the application to
continue as if it had called the Vulkan driver directly.

This repository currently contains the starting framework for that work. The
initial source tree focuses on establishing clear ownership boundaries before
the full API surface is generated or implemented.

## Repository Layout

```text
src/vkfwd/core/       Shared core library source, generated command code, hooks.
src/vkfwd/forwarder/  Vulkan layer implementation loaded into source apps.
src/vkfwd/receiver/   Receiver-side pipeline and replay scaffolding.
src/third_party/  Vendored third-party code and pinned Vulkan spec inputs.
dev/test/         Development tests and test harnesses.
dev/env/          Coding environment setup scripts.
dev/bin/          Helper scripts and utilities.
```

The source layout is documented in `doc/repository-structure.md`. In that
model, the shared core static library
contains pack/unpack, endpoint contracts, transport interfaces, protocol code,
generated command code, hooks, and common utilities. The forwarder shared
library, receiver executable, recorder layer, and saved-stream replay tool are
thin role-specific targets that link the core library.

Everything under a `generated/` source tree is produced by
`dev/generator/vulkan_metadata.py` or another explicit generator entry point and
may be replaced by regeneration. Manual code belongs outside generated trees.
Generated pack/unpack command code and per-command metadata live under
`src/vkfwd/core/generated/command/`. Forwarder-specific generated loader,
dispatch, and interceptor glue should live under
`src/vkfwd/forwarder/generated/`. Generated roots may contain small manifests
for provenance and versioning, but not one centralized all-API metadata blob.
Per-command manual hooks live under `src/vkfwd/core/hook/<api>Hook.hpp`;
generated command code conditionally includes those files when present. Hook
implementations that need out-of-line bodies may add a matching `.cpp` file and
wire it into CMake manually. See `src/vkfwd/core/generated/README.md` and
`src/vkfwd/core/hook/README.md` for the folder ownership rules.

## Development Rule

Comments in this repository should emphasize why the code exists, what
assumptions it relies on, and what invariants callers must preserve. Avoid
comments that only restate what the code already says. See
`CONTRIBUTING.md` for the human-facing project comment rule and `AGENTS.md`
for the agent-facing rule.

## Architecture Sketch

The capture/replay path is expected to evolve around five components:

1. Forwarder Vulkan entry points exported from `src/vkfwd/forwarder/layer.cpp`.
2. Generated or hand-written interceptors that capture every Vulkan parameter
   before forwarding the call to the next Vulkan implementation.
3. A serializer that deep-copies Vulkan structs, arrays, handles, `pNext`
   chains, memory payloads, and call ordering into a stable wire format.
4. An API endpoint interface that accepts serialized calls, executes them
   locally or remotely, and completes the caller-visible return value, output
   parameters, and handle mapping before the intercepted API call returns.
5. A receiver/replay runtime that deserializes calls, maps source handles to
   receiver handles, reconstructs state, and invokes the real Vulkan API.

The module split is:

- `vkfwd_core`: shared generated pack/unpack, endpoint, protocol, and utility
  code.
- `vkfwd_forwarder`: loadable Vulkan layer shared object for forwarding.
- `vkfwd_receiver`: receiver-side deserialization and replay helpers.

The first implementation milestone is a loadable explicit Vulkan layer that can
be discovered by the Vulkan loader and can trace a small set of calls while
passing them through locally. From there, the project can grow toward generated
dispatch tables and complete Vulkan API coverage.

Important early work:

- Build complete Vulkan dispatch interception for instance and device commands.
- Generate parameter metadata from Vulkan XML instead of manually duplicating
  the whole API surface.
- Define serialization rules for structs, optional pointers, arrays, `pNext`
  chains, handles, host memory payloads, and externally synchronized objects.
- Add receiver-side deserialization and replay with source-to-destination handle
  mapping.

Design notes:

- `doc/repository-structure.md` describes the intended top-level source and
  target split.
- `doc/vulkan-coverage-plan.md` tracks the overall Vulkan API coverage plan.
- `doc/api-pack-unpack-design.md` describes the generated API pack/unpack path,
  stream compatibility, ownership rules, and manual hook contract.
- `doc/todo.md` tracks known unfinished design and implementation follow-ups.

## Build

The scaffold uses CMake and expects Vulkan headers and loader development files
to be available on the system.

```sh
dev/bin/build.py d
ctest --test-dir build/linux.gcc.debug
```

Build variants are `d` for debug, `r` for release, `p` for profiling
(`RelWithDebInfo`), and `c` for cleanup. Android builds use `--android` and
write to a separate `build/android-<abi>.clang.<variant>` folder.

For local development shell setup, source:

```sh
. setup.sh
```

## Status

This is a new project skeleton. Complete Vulkan API interception, parameter
serialization, receiver-side deserialization/replay, generated dispatch tables,
and concrete API endpoint implementations are not implemented yet.

## License

MIT. See `LICENSE`.
