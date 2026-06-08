# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Test

```sh
dev/bin/build.py d          # debug build  → build/linux.gcc.debug/ (or build/macos.clang.debug/)
dev/bin/build.py r          # release build
dev/bin/build.py p          # profiling (RelWithDebInfo)
dev/bin/build.py c          # clean

dev/bin/cit.py                               # format-check then run all tests (default: build/linux.gcc.debug)
dev/bin/cit.py --build-dir build/macos.clang.debug   # specify build dir
```

`cit.py` runs `dev/bin/format-all-sources.py --check` first, then invokes the `vkfwd_internal_tests` Catch2 binary directly (selecting the newest binary if multiple configs are present). To pass Catch2 filters (e.g., run a single tag), run the test binary directly:

```sh
./build/linux.gcc.debug/dev/test/internal-test/vkfwd_internal_tests "[loopback]"
```

Build writes to `build/<platform>.<compiler>.<variant>/`. All tests are compiled into a single `vkfwd_internal_tests` binary using Catch2. Test sources are discovered via `internal-test.cmake` manifests scattered across the source tree.

Before editing `src/vkfwd/ferry/`, read `src/vkfwd/ferry/README.md`. For area-specific changes also read the local README under `core/`, `forwarder/`, or `receiver/`. Update any stale README content in the same change as the code.

## Architecture

### Implementation Families

`src/vkfwd/` is split into two families that must not mix runtime assumptions:

- **`ferry/`** — current mechanical per-API-call forwarding path. Owns the generated command model, pack/unpack code, forwarder layer, receiver/replay scaffolding, generator scripts, and tests.
- **`facade/`** — placeholder for a future stateful Vulkan front end with local handle identities. Currently empty.

### Runtime Roles

- **Interceptor (forwarder)** — Vulkan explicit layer loaded into the source application. Owns loader entry points, dispatch-table chaining, and source-visible API completion. Does not call the local Vulkan driver and holds no per-instance/per-device dispatch chains.
- **Receiver** — destination-side process that unpacks forwarded command payloads, maps source handles to receiver handles, and replays against the local Vulkan implementation.

### CMake Targets

- `vkfwd_core` — static library: generated pack/unpack, transport contracts, protocol, blob storage, hooks, utilities. Linked by both forwarder and receiver.
- `vkfwd_forwarder` — shared library loaded by the Vulkan loader.
- `vkfwd_receiver` — receiver-side replay scaffolding.

### Key Source Areas

| Path | Owns |
|---|---|
| `ferry/core/blob.hpp` | Grow-only byte arena with stable logical offsets and bounded views (`SafeArrayView`). |
| `ferry/core/command_stream.hpp` | Stream magic, schema version, command chunk header layout, and stream storage API. |
| `ferry/core/transport_session.hpp` | Forwarder-side session boundary: `send_accumulated_api_calls()`. |
| `ferry/core/receiver_session.hpp` | Receiver-side demultiplexing and responder lifecycle. |
| `ferry/core/generated/` | Generator-owned: command pack/unpack, structure helpers, dispatch-table types, `vulkan_api.hpp` API version metadata. |
| `ferry/core/hook/` | Human-owned: per-command customization headers (`<api>Hook.hpp`) and optional `.cpp` bodies. |
| `ferry/forwarder/layer.cpp` | Exports `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` to the Vulkan loader. |
| `ferry/forwarder/forwarder.hpp` | Thread-local `Forwarder`: request blob, source-thread token, shared `TransportSession`. |
| `ferry/forwarder/generated/` | Generator-owned: layer entry-point wrappers (`_entry` suffix), dispatch-table instances. |
| `ferry/receiver/` | Receiver scaffolding: `Receiver`, `ReplayContext`, generated dispatch. |
| `ferry/test/` | In-process loopback tests binding forwarder to receiver without a socket. |
| `ferry/script/generator/vulkan_metadata.py` | Source of all generated code. Regenerate instead of hand-editing. |

### Forwarding Data Flow

1. Source application calls a Vulkan API. The Vulkan loader routes it to the forwarder's `_entry` wrapper (in `forwarder/generated/entry/`).
2. The wrapper optionally runs a `before_pack` hook, copies arguments into a `Command::Parameters` struct, and appends a command chunk to the thread-local `Forwarder::request_blob()`.
3. Response-bearing commands call `Forwarder::flush()`, which calls `TransportSession::send_accumulated_api_calls()` with the accumulated blob (source-thread token prefix + zero or more deferrable chunks + the flush-triggering chunk).
4. On the receiver side, `ReceiverSession` demultiplexes by source-thread token, dispatches command chunks by command id, runs receiver adapters (unpack → replay → response pack), and returns the response blob.
5. The forwarder wrapper unpacks the response blob, writes output parameters back to the caller, and returns `VkResult`.

### Blob / Serialization Invariants

Pointers in packed payloads are **never source-process addresses**:
- Command parameter pointer slots use **command-relative offsets** (base = start of the command chunk where the stable command id is stored).
- Structure pointer slots use **structure-relative offsets** (base = start of that structure chunk where `sType` is stored).
- Null source pointers encode as zero offsets. Real offsets are always nonzero because referenced data follows the fixed header.
- Nested structure pointer slots remain relative to that nested structure, not the outer command.

`Blob` invariants to preserve when modifying `blob.hpp`/`blob.cpp`:
- `grow()` is the only append operation — returns a bounded `SafeArrayView` over exactly the new bytes.
- `data_at()` is the only read operation — returns `SafeArrayView<const uint8_t>`.
- No random-offset mutable writes. Patch pointer fields through typed pointers retained from `grow()`.
- `flatten()` collapses multi-chunk blobs into a single contiguous allocation for transports that need it.

### Generated vs. Manual Code

**Never hand-edit files under `generated/` trees.** Update `ferry/script/generator/vulkan_metadata.py` and regenerate:

```sh
# from repo root — regenerate all generated files
python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py
```

**Manual hook code** lives exclusively under `ferry/core/hook/<api>Hook.hpp` (and optional `.cpp`). Generated command files conditionally include hooks when present. The generator must never create, overwrite, or delete files in `hook/`.

### Transport and Module Boundaries

Strictly enforced ownership:
- **Forwarder** — no replay logic, no local Vulkan dispatch, no source-to-destination handle maps.
- **Transport layer** — no Vulkan command semantics beyond framing, routing, and failure propagation.
- **Receiver/replay** — owns destination Vulkan dispatch tables, source-to-receiver handle maps, replay ordering, and response payload construction. Handle maps must never use raw source-process pointer values on the receiver.

`pNext` chains: generated structure code only packs known `sType` values. Unknown structures are rejected (not copied opaquely) because they may contain source pointers, callbacks, or handles meaningless on the receiver. The validator checks the full chain before copying any node.

### Testing Pattern

Tests use in-process loopback transport (`ferry/test/loopback_session.*`) — no socket or process boundary required. To add a test file, create it in the relevant source directory alongside an `internal-test.cmake` manifest that lists it in `VKFWD_INTERNAL_TEST_LOCAL_SOURCES`. The build system discovers manifests repo-wide automatically.

Generated forwarder tests (under `forwarder/generated/test/`) install a test transport session, call the generated Vulkan entry point, validate the request blob, and return a synthesized response blob. This tests the full entry-point path, not just pack/unpack helpers in isolation.

## Commenting Rule

Comments must explain **why**: design invariants, Vulkan loader-chain constraints, pointer/ownership lifetime assumptions, placeholder behavior that must not be confused with complete forwarding or replay, and replay-ordering requirements. Do not write comments that restate the code.

For Vulkan interception and replay code specifically, call out: loader-chain and dispatch-table invariants; pointer, array, and `pNext` lifetime expectations; pack/unpack ownership for borrowed application memory; source-to-receiver handle mapping assumptions; replay ordering and externally synchronized state.
