# Receiver Hook Framework and Endpoint Grouping — Design

## Status

Design for the parallel-work overhaul of the receiver-side generated code:

1. Introduce an **opt-in per-API hook framework** on the receiver side, mirroring
   the forwarder's `__has_include` / `CommandHooks<...>` mechanism.
2. **Split** `src/vkfwd/ferry/receiver/generated/endpoints.cpp` so it contains
   only the `call_api_endpoint(...)` dispatcher; per-API endpoint
   implementations move into per-group files under
   `receiver/generated/endpoint/`.
3. Migrate the two existing receiver-side customizations
   (`RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` and `receiver_dispatch_update_lines`)
   into hook headers, then delete both generator-internal mechanisms.

This work is a hard dependency for memory-map manager phase 1 (see
`doc/memory_map_management.md`). Memory-map manager phase 0
(skeleton refactor) is independent and can proceed in parallel.

## Why

### Asymmetry today

| Mechanism | Forwarder | Receiver |
|---|---|---|
| External opt-in hook header (`__has_include`) | Yes (`forwarder/hook/<api>ForwarderHook.hpp`) | **No** |
| Hand-curated full-override set inside the generator | No | **Yes** (`RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` in `vulkan_metadata.py:1747`) |
| Hard-coded per-API inline emissions inside the generator | No | **Yes** (`receiver_dispatch_update_lines` in `vulkan_metadata.py:1889`, currently customizing `vkCreateInstance` / `vkCreateDevice`) |

The two receiver-side mechanisms both live inside `vulkan_metadata.py`. Every
new per-API customization has to land as a generator edit, not as a self-contained
human-written hook file. That violates the project rule that humans own hook
code and the generator owns generated code; over time it pushes more hand
logic into the generator and makes per-API behavior harder to find, harder to
review, and harder to test in isolation.

### Single-file endpoint emission

`receiver/generated/endpoints.cpp` today contains every receiver endpoint
function plus the `call_api_endpoint(...)` dispatcher in one file. The
current supported slice is ~35 commands and the file is already 718 lines.
Expanding to the full Vulkan API (~600+ commands) is not viable in one file —
build time, source readability, and diff review all degrade.

The repo already documents an API grouping policy in
`src/vkfwd/ferry/core/README.md` and `wip.md`: ~25-60 commands per group, hard
split before a group reaches ~75. Generated command implementation sources and
test sources are slated to follow the same policy. Receiver endpoint sources
should not be the exception.

## Goals

- Every per-API customization the receiver can need is opt-in via a hook
  header file alongside the existing `core/hook/` and `forwarder/hook/`
  patterns.
- The generator stops carrying per-API customization data
  (`RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`, `receiver_dispatch_update_lines`)
  once the migration is complete.
- The receiver-side generated tree scales to the full Vulkan API surface
  without any single source file growing past the documented grouping limit.
- `endpoints.cpp` becomes a dispatcher-only file that doesn't change shape
  when API coverage grows — only the per-group implementation files grow.

## Non-goals

- Refactoring the forwarder-side hook framework. It already works the way we
  want; this spec brings the receiver up to parity, it does not redo the
  forwarder.
- Defining the API group taxonomy itself. The grouping policy from
  `core/README.md` and the documented domains (instance/device bring-up,
  buffer/memory, descriptor, pipeline, etc.) are inherited as-is. Concrete
  group assignment lives in the generator's group manifest, not in this
  spec.
- Changing the dispatcher representation. Today it is a switch over
  `CommandId`; the compiler turns dense small enums into a jump table, which
  is already O(1). A future hash-table swap is not required for this work
  and can be revisited if profiling shows it matters.
- Touching the core pack/unpack hook layer (`core/hook/`). It is zero
  runtime cost, it is not in the way of any of the changes in this spec,
  and it stays as it is.

## Receiver hook interface

New namespace and template, parallel to `vkfwd::forwarder::manual::CommandHooks`:

```cpp
namespace vkfwd::receiver::manual {

// Per-API customization for receiver-side endpoints. Hook authors specialize
// this template in receiver/hook/<api>ReceiverHook.hpp. The generated endpoint
// stub conditionally calls into each phase via `if constexpr (Hooks::<phase>_enabled)`.
template <::vkfwd::generated::CommandId>
struct CommandHooks {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = false;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template <class... Args> static constexpr void before_unpack(Args &...) noexcept {}
    template <class... Args> static constexpr void before_call(Args &...) noexcept {}
    template <class... Args> static constexpr void after_call(Args &...) noexcept {}
    template <class... Args> static constexpr void before_pack_response(Args &...) noexcept {}
    template <class... Args> static constexpr void after_pack_response(Args &...) noexcept {}

    // When replace_endpoint_enabled is true the generated stub bypasses the
    // standard unpack/call/pack body entirely and forwards the whole endpoint
    // call to this method. Used when the endpoint's behavior cannot be expressed
    // as parameter mutation around a real Vulkan call (e.g., memory-map
    // delegation, where there is no real Vulkan call at all on this side).
    template <class... Args> static constexpr bool replace_endpoint(Args &...) noexcept { return false; }
};

} // namespace vkfwd::receiver::manual
```

Phase signatures (typed, not the variadic placeholders shown above) when
`<api>_enabled = true`:

| Phase | Signature | Use case |
|---|---|---|
| `before_unpack` | `(SafeArrayView<uint8_t> request_view, ReplayContext &)` | Validate or transform raw request bytes before typed unpack |
| `before_call` | `(Command::Parameters &, ReplayContext &)` | Mutate parameters before the real Vulkan call (rare) |
| `after_call` | `(const Command::Parameters &, Command::Response &, ReplayContext &)` | Post-call state updates — this is what subsumes `receiver_dispatch_update_lines` |
| `before_pack_response` | `(const Command::Parameters &, Command::Response &, ReplayContext &)` | Final tweaks to the response before serialization |
| `after_pack_response` | `(const Command::Parameters &, CommandStream & response_stream)` | Append protocol payloads after the typed response |
| `replace_endpoint` | `(const CommandStream &, const Range &, CommandStream &, ReplayContext &) -> bool` | Full override — subsumes `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` |

`replace_endpoint` returning `bool` mirrors the existing endpoint contract
(`return false` propagates to the caller as a replay failure). When
`replace_endpoint_enabled = true` it is invoked instead of the generated body
— the other phase flags are ignored.

### Hook file convention

```
src/vkfwd/ferry/receiver/hook/<api>ReceiverHook.hpp   // template specialization
src/vkfwd/ferry/receiver/hook/<api>ReceiverHook.cpp   // optional .cpp body
```

Matching the existing `core/hook/` and `forwarder/hook/` conventions. The
generator emits `#if __has_include("hook/<api>ReceiverHook.hpp")` in each
per-API endpoint .cpp file so hooks are pure opt-in.

## Endpoint grouping

### New file layout

```
src/vkfwd/ferry/receiver/generated/
    endpoints.hpp                       # public declarations for call_api_endpoint
    endpoints.cpp                       # ONLY call_api_endpoint() + its dispatch
    endpoint/
        <group>.hpp                     # per-group endpoint function declarations
        <group>.cpp                     # per-group endpoint function implementations
        test/                           # generated per-group endpoint tests if applicable
```

`endpoints.cpp` after the migration is a flat switch over `CommandId` that
forwards to the per-group functions. The dispatcher does not grow as new
commands are added — only the per-group `.cpp` files do.

`endpoints.hpp` may stay as a single header that includes the per-group headers,
or be replaced by per-group `endpoint/<group>.hpp` files. Either is fine; the
dispatcher only needs the function declarations to be available.

### Group assignment

The group manifest lives where the rest of the generator's grouping data lives.
Today the policy is documented (`core/README.md`, `wip.md`) but not encoded;
this work needs to either:

- introduce a small group-manifest file (e.g.,
  `script/generator/api_groups.py` or a JSON sibling), or
- make the existing grouping-policy comment authoritative by encoding the
  assignment table in `vulkan_metadata.py`.

Either choice should match whatever the parallel command/structure grouping
migration ends up using. Coordinate with whoever does that migration so the
two paths share a single manifest, not two independent ones.

### Dispatcher

`call_api_endpoint(...)` stays a `switch` over `CommandId`. Switch over a
dense small enum compiles to a jump table on every supported toolchain and
gives O(1) dispatch with no allocation. A hash-table or function-pointer
table is not required and is left out of scope; revisit if profiling shows a
measurable cost.

## Migration plan

The migration happens in three steps. Each step lands as one reviewable
change. After step 3 the generator no longer carries either
`RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` or `receiver_dispatch_update_lines`.

### Step 1 — introduce the hook framework

- Add `vkfwd::receiver::manual::CommandHooks<>` declaration. Generator wires
  `__has_include("hook/<api>ReceiverHook.hpp")` and the six `if constexpr`
  blocks into every receiver endpoint emission.
- All `<phase>_enabled` constants default to `false`, so generated behavior is
  byte-for-byte unchanged.
- `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` and `receiver_dispatch_update_lines`
  are still in place; this step adds the new mechanism without removing the
  old.
- Regression test: existing receiver behavior is unchanged (full
  `dev/bin/cit.py` green).

### Step 2 — split endpoints.cpp by API group

- Generator emits one `.cpp` per group under `receiver/generated/endpoint/`.
- `endpoints.cpp` shrinks to the dispatcher.
- Same group manifest as the command source migration uses.
- No semantic change — purely a structural / file-layout change.
- Regression test: still green.

### Step 3 — migrate existing customizations into hook headers

Three hook files land:

- `receiver/hook/vkCreateInstanceReceiverHook.hpp` — `after_call_enabled = true`,
  `after_call` performs the `replay_context.dispatch.instance.init(...)` call
  that today comes from `receiver_dispatch_update_lines`.
- `receiver/hook/vkCreateDeviceReceiverHook.hpp` — same pattern for
  `replay_context.dispatch.device.init(...)`.
- `receiver/hook/vkUnmapMemoryReceiverHook.hpp` —
  `replace_endpoint_enabled = true`, body logs an error and returns
  `false`. The memory-map design (`doc/memory_map_management.md`) routes
  the real unmap path through `manual::CommandId::MemoryUnmap` and
  treats any standard generated `vkUnmapMemory` chunk as a protocol
  violation that must not silently call the real driver. The hook
  replaces the matching `receiver_memory_map_endpoint_source` emission
  without preserving its behavior — that emission's path is no longer
  the supported one.
- `receiver/hook/vkMapMemoryReceiverHook.hpp` —
  `replace_endpoint_enabled = true`, same fail-closed body for the
  standard generated `vkMapMemory` chunk. Added by the memory-map
  skeleton work (see below) for symmetry with the unmap hook.

Then delete the generator-side mechanisms:

- Remove `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`.
- Remove `receiver_memory_map_endpoint_source`.
- Remove `receiver_dispatch_update_lines`.

After this step the generator has one path for every receiver endpoint, plus
the optional opt-in hook calls. Per-API customization data has fully left
the generator.

## What depends on this work

### Memory-map manager — phase 1 (and later)

Memory-map manager phase 1 wants the receiver's `vkAllocateMemory` and
`vkFreeMemory` standard generated endpoints to feed
`MemoryMapReceiver`'s per-handle allocation registry as a side effect of
normal replay, and it wants the standard generated `vkMapMemory` /
`vkUnmapMemory` endpoints to **fail closed** so they never silently
drive the real driver (the supported path for those two is the manual
`vkfwd::manual::CommandId::MemoryMap` / `MemoryUnmap` protocol; see
`doc/memory_map_management.md`).

- Before this work: that pattern would have meant four entries in
  `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`, two for the bookkeeping
  endpoints and two for the disabled standard map/unmap paths.
- After this work: the bookkeeping for `vkAllocateMemory` /
  `vkFreeMemory` lands as `after_call` hook headers on those two
  commands; the disabled standard `vkMapMemory` / `vkUnmapMemory` paths
  land as `replace_endpoint` hooks that log and return `false`. The
  manager-side code stays the same.

Memory-map manager **phase 0** (skeleton refactor) depends on this work only
to the extent that it ships two fail-closed receiver hooks
(`vkMapMemoryReceiverHook.hpp`, `vkUnmapMemoryReceiverHook.hpp`) using the
`replace_endpoint` slot — the receiver-side per-handle map itself stays
empty, and no other receiver endpoint behavior changes. The hook framework
must therefore exist before memory-map phase 0 lands.

### Future per-API receiver customization

Any future per-API receiver customization (e.g., handle-map updates around
create/destroy commands as those land, replay-state bookkeeping around
pipeline creation) lands as a hook header instead of a generator edit. The
existing `vkCreateInstance` and `vkCreateDevice` cases become the worked
examples for that pattern.

## Test plan

Tests land alongside the real behavior they cover, not against empty
scaffolding.

### Step 1 — hook framework introduced

- Existing tests must continue to pass with all hook flags defaulting to
  `false`. That is the regression bar; no new tests are added in step 1.

### Step 2 — endpoints split

- Existing tests must continue to pass. Build-system change is covered by the
  build itself; if the per-group files compile and link, the split is correct.
- Optional: a compile-time check that the dispatcher's `switch` cases match
  the declared per-group function names. May be unnecessary if the generator
  itself owns both sides.

### Step 3 — migrations

- `vkCreateInstance` / `vkCreateDevice` after-call dispatch updates: covered
  by any test that exercises receiver-side dispatch table initialization
  (the in-process loopback tests already do this).
- `vkUnmapMemory` replacement: existing memory-map tests cover the current
  delegation behavior; they must still pass after the migration.
- New focused test: a synthetic hook header that toggles each `<phase>_enabled`
  flag and asserts the generated endpoint calls each phase the expected
  number of times in the expected order. Lives under
  `receiver/generated/endpoint/test/` and is the only piece of test code
  this work adds beyond regression coverage.

## Generator module organization

This work materially increases the amount of generator code under
`src/vkfwd/ferry/script/generator/vulkan_metadata.py`. The current file
already mixes metadata loading, command emission, structure emission, dispatch
emission, forwarder emission, receiver emission, manifest emission, and test
emission. The grouping policy notes in `wip.md` flag this as expected
technical debt that gets paid down when the generator becomes too large to
maintain as one file.

If, during step 1 or step 2 above, the file passes the "one human can hold
it in their head" threshold (roughly when the receiver-emission section
doubles in size to accommodate hook plumbing and per-group output), split the
generator into modules along the boundaries already documented in `wip.md`:

- `vulkan_metadata.py` — metadata parsing/modeling (top of pipeline), entry
  point, and shared utilities.
- `commands.py` — command pack/unpack emission and command tests.
- `structures.py` — structure serializer emission and structure tests.
- `forwarder.py` — forwarder entry-point emission and forwarder tests.
- `receiver.py` — receiver endpoint emission and per-group split logic.
- `manifest.py` — CMake manifest and `vulkan_manifest.json` emission.

Split decisions are at the implementer's discretion: if the file is still
readable after step 1, defer; if it's painful after step 2, split. Don't
split preemptively before this work makes it necessary, and don't keep the
file monolithic out of inertia if it becomes a real obstacle. Whatever the
split looks like, it must not change the generated output — a regression run
before and after the split should produce byte-for-byte identical files in
all generated trees.

## Open questions

1. **Group manifest sharing.** This work assumes the command/structure source
   grouping migration owns the group manifest. If both happen at once and
   neither has shipped yet, one of them needs to ship the manifest first.
   The receiver split should not invent a second, parallel manifest.

2. **Hook header naming.** `<api>ReceiverHook.hpp` parallels
   `<api>ForwarderHook.hpp`. An alternative is `<api>EndpointHook.hpp` to
   emphasize the receiver-endpoint phase model (which is conceptually
   different from the forwarder's entry-point phases — different phase names,
   different signatures). Naming consistency vs. phase-model honesty —
   either is defensible. Current draft picks consistency.

3. **`replace_endpoint` semantics.** Whether `replace_endpoint` should be
   able to coexist with other phases (e.g., still run `before_unpack` for
   validation) or be a strict mutual-exclusion override. Current draft makes
   it strict — when `replace_endpoint_enabled` is `true` no other phase
   fires. Simpler to reason about; revisit if a real use case wants both.
