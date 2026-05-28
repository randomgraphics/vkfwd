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

Forwarding to another process or remote machine is intentionally a thin
boundary for now. The project starts with two main modules plus an abstract
handoff interface so the capture/replay work is not coupled to IPC, sockets, or
any specific transport.

The intended long-term shape is:

- `vkfwd_layer`: a Vulkan explicit layer that intercepts instance, device,
  queue, command buffer, memory, synchronization, and presentation calls, then
  serializes every parameter needed to replay them.
- `vkfwd_receiver`: a receiver library/runtime that deserializes the captured
  stream, reconstructs Vulkan object state, and invokes actual Vulkan API calls
  on the receiving side.
- A forwarding sink interface that can later be implemented by local IPC,
  remote network transports, capture files, or in-process test harnesses.

This repository currently contains the starting framework for that work. The
initial source tree focuses on establishing clear ownership boundaries before
the full API surface is generated or implemented.

## Repository Layout

```text
src/vkfwd/      Core vkfwd source files and Vulkan layer entry points.
src/contrib/    Third-party code referenced directly by the main vkfwd code.
dev/test/       Development tests and test harnesses.
dev/env/        Coding environment setup scripts.
dev/bin/        Helper scripts and utilities.
```

## Development Rule

Comments in this repository should emphasize why the code exists, what
assumptions it relies on, and what invariants callers must preserve. Avoid
comments that only restate what the code already says. See
`CONTRIBUTING.md` for the human-facing project comment rule and `AGENTS.md`
for the agent-facing rule.

## Architecture Sketch

The capture/replay path is expected to evolve around five components:

1. Vulkan layer entry points exported from `src/vkfwd/layer.cpp`.
2. Generated or hand-written interceptors that capture every Vulkan parameter
   before forwarding the call to the next Vulkan implementation.
3. A serializer that deep-copies Vulkan structs, arrays, handles, `pNext`
   chains, memory payloads, and call ordering into a stable wire format.
4. A forwarding sink interface that accepts serialized calls without caring
   whether they go to IPC, a remote socket, a file, or a test double.
5. A receiver/replay runtime that deserializes calls, maps source handles to
   receiver handles, reconstructs state, and invokes the real Vulkan API.

The module split is:

- `vkfwd_capture`: layer-side capture and serialization helpers.
- `vkfwd_layer`: loadable Vulkan layer shared object.
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

## Build

The scaffold uses CMake and expects Vulkan headers and loader development files
to be available on the system.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

For local development shell setup, source:

```sh
. dev/env/setup.sh
```

## Status

This is a new project skeleton. Complete Vulkan API interception, parameter
serialization, receiver-side deserialization/replay, generated dispatch tables,
and concrete forwarding transports are not implemented yet.

## License

MIT. See `LICENSE`.
