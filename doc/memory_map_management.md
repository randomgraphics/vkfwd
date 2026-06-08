# Memory Map Management Design

This document is the single canonical design for vkfwd's mapped-memory
handling. It covers both the **skeleton framework** (phase 0, the structural
shape this work is currently landing) and the **per-strategy implementations
and wire protocol** that follow in later phases.

For broader context — the multi-phase plan, sync-point obligations, and why
this subsystem can't follow the standard generated endpoint pattern — see
`src/vkfwd/ferry/core/README.md`.

## Status

Phase 0 (skeleton framework) is in design. Phase 1+ (real per-strategy
behavior and the staged-byte wire protocol) is sketched here but not
implemented.

## Dependencies

- **Phase 0** depends on nothing else. It can be implemented in parallel with
  any other current work in the repo.
- **Phase 1 and later** depend on a receiver dispatch path for vkfwd-owned
  custom command ids and on the receiver-side handle/allocation state needed to
  translate source-visible memory handles into destination Vulkan memory. The
  public Vulkan `vkMapMemory` / `vkUnmapMemory` entry points are still exposed
  by the forwarder layer, but their wire payloads are custom memory-map commands
  rather than generated Vulkan `CommandId::MapMemory` /
  `CommandId::UnmapMemory` chunks.

Phase 0 deliberately does not delegate receiver-side allocate/free endpoints
through the manager, precisely so this dependency does not block it. Standard
generated receiver map/unmap endpoints are not the intended long-term path; they
remain compiled only as generated Vulkan coverage while the custom command path
is brought up. See [Receiver factory deferral](#receiver-factory-deferral).

## Why vkMapMemory and vkUnmapMemory Are Special

Every other Vulkan command in the current supported slice takes input data,
sends it over the wire, and returns scalar outputs (handles, results,
properties). `vkMapMemory` is different: it returns a host-accessible pointer
that the application writes to directly, and those writes must eventually
reach receiver-side Vulkan device memory.

Returning the receiver-side mapped pointer to the source application is
**always wrong**, even when it appears to work in a local loopback setup:

- The receiver pointer is invalid in the source process's virtual address
  space.
- The receiver pointer's lifetime is owned by receiver-side Vulkan; the
  source app cannot control it.
- Source-process writes through that pointer would fault or corrupt
  unrelated receiver memory.
- Source-process reads would not see receiver-side Vulkan writes.
- Synchronization semantics (`vkFlushMappedMemoryRanges`,
  `vkInvalidateMappedMemoryRanges`) would be undefined.

The correct design interposes source-side staging memory and transfers bytes
explicitly across the address-space boundary.

## Overall Design

### Core architectural decision — per-strategy polymorphic handling

Each live `VkDeviceMemory` allocation owns one dedicated
`ForwarderAllocation` / `ReceiverAllocation` instance, chosen at
`vkAllocateMemory` time from the memory type's `VkMemoryPropertyFlags` and
fixed for that allocation's lifetime. **Non-coherent and coherent
allocations never share code paths beyond the abstract base interface** —
each strategy is its own subclass pair, in its own file, with its own
state.

This is the central design decision and the reason for the polymorphic
framework. The alternative ("one class with internal `if (coherent)`
branches") was rejected because the algorithms diverge in too many
dimensions: when to copy bytes, what goes on the wire, which Vulkan API
boundaries are sync points, what per-allocation state must be tracked.
Mixing them in one class makes both harder to read and harder to test, and
makes any future per-strategy variation a new branch in code that already
has too many branches.

Concretely, this design lets each strategy be:

- written by one author who only needs to reason about one memory type at a
  time,
- tested in isolation with strategy-specific unit tests,
- replaced or extended without touching the manager, the hooks, the
  receiver session, or any other strategy.

### Non-coherent — implementation alternatives

Vulkan requires applications to call `vkFlushMappedMemoryRanges` and
`vkInvalidateMappedMemoryRanges` to make non-coherent host writes / GPU
writes visible across the boundary. That pins down most of the semantics
but still leaves several implementation choices for *when* vkfwd transfers
bytes.

**Chosen: N2 — explicit flush/invalidate, no transfer at map/unmap.**
The remaining options below are documented as design alternatives that
were considered and rejected. See the rationale at the end of this
subsection.

#### N1 — Whole-range transfer at unmap only

| | |
|---|---|
| `vkMapMemory` | Allocate staging buffer, return source pointer. No data transfer. |
| `vkUnmapMemory` | Copy whole staging buffer to receiver, then real unmap. |
| `vkFlushMappedMemoryRanges` | Not implemented (or treated as deferred-until-unmap). |
| `vkInvalidateMappedMemoryRanges` | Not implemented — GPU readback unsupported. |
| **Pros** | Smallest wire protocol. Works without phase 2 commands existing. Simplest possible end-to-end map/unmap. |
| **Cons** | Apps that call flush expect it to take effect immediately, not at unmap. Apps using invalidate (GPU readback) get nothing. Not fully Vulkan-spec compliant. Pessimistic — always copies the full range even if the app barely touched it. |

#### N2 — Explicit flush/invalidate, no transfer at map/unmap (chosen)

| | |
|---|---|
| `vkMapMemory` | Allocate staging. No data transfer. |
| `vkUnmapMemory` | Free staging. No data transfer. |
| `vkFlushMappedMemoryRanges` | Copy specified ranges source → receiver. |
| `vkInvalidateMappedMemoryRanges` | Copy specified ranges receiver → source. |
| **Pros** | Matches Vulkan spec exactly. Only transfers what the app explicitly asks for — efficient. Per-range transfers. |
| **Cons** | App that forgets to flush before unmap silently loses data. Strict reading: app violated the spec, vkfwd is correct — but unforgiving. Requires phase 2 entry points to be useful at all. |

#### N3 — Implicit unmap flush + explicit flush/invalidate with synced-range tracking (rejected)

| | |
|---|---|
| Per-allocation state | A `synced_ranges` set of `(offset, size)` intervals that are currently in sync between source staging and receiver memory. Initially empty. |
| `vkMapMemory` | Allocate staging. `synced_ranges` stays empty. No data transfer. |
| `vkFlushMappedMemoryRanges` | For each input range: copy source → receiver, then add the range to `synced_ranges` (coalescing adjacent intervals). |
| `vkInvalidateMappedMemoryRanges` | For each input range: copy receiver → source, then add the range to `synced_ranges`. |
| `vkUnmapMemory` | Compute `pending = [0, allocation_size) \ synced_ranges`. Copy each pending sub-range source → receiver, then real `vkUnmapMemory`. Free staging. |
| **Pros** | More forgiving than N2 for apps that forget to flush uploads before unmap — we push their bytes. Avoids re-pushing bytes the app already flushed explicitly. |
| **Cons** | **Corrupts GPU output for download-only patterns.** If the app maps non-coherent memory to read GPU-written data, invalidates a sub-range, reads it, and unmaps without ever touching the rest of the allocation, N3's `pending = [0, allocation_size) \ synced_ranges` set is non-empty — and source-side staging for that pending region is uninitialized. Pushing those bytes at unmap **overwrites real GPU output** in the receiver allocation outside the invalidated sub-range. The forwarder cannot distinguish "host wrote but forgot to flush" from "host never touched" without dirty tracking, so the implicit unmap push helps one case and silently corrupts the other. Also has the separate write-after-flush limitation (writing → flushing → writing again without re-flush loses the second write at unmap). Per-allocation interval set adds bookkeeping that, in practice, exists only to drive an unsafe push. |

#### N4 — N2 + page-protection dirty tracking

| | |
|---|---|
| Staging | Builds on the reserve+commit VM scaffolding already shipped for N2 (see "Source-Side Staging: VM Reserve + Commit"). Committed pages start at `PROT_NONE` / `PROT_READ`; a fault handler flips them to `PROT_READ \| PROT_WRITE` and marks them dirty on first CPU write. At unmap, the dirty set tells the forwarder exactly which bytes the host actually wrote. |
| `vkUnmapMemory` | Push only the dirty-page subset of the active mapped range, minus the sub-ranges the app already flushed, source → receiver, then real unmap. The "What goes on the wire" rule still applies — committed slivers outside the mapped range are never transmitted even if dirty. Untouched pages within the mapped range are never pushed, so download-only patterns stay safe. |
| **Pros** | Minimum bytes on the wire. Best perf for sparse writes or sparse reads. Particularly useful for large mappings where most of the range is untouched. **Re-enables a safe implicit unmap flush** — the forgotten-flush-on-upload case that N2 surrenders gets handled correctly, without N3's readback corruption, because dirty tracking distinguishes "host wrote" from "host never touched." Incremental on top of N2 — only the fault handler and dirty-page accounting are new; reserve, commit, page-rounding, and release are already in place. |
| **Cons** | Platform-specific fault-handler surface (Linux `SIGSEGV`, Windows VEH). Interacts with debuggers, async-signal-safety rules, and other layers that handle the same signals. Larger correctness surface than an optimization-only feature. |

### Why N2 (chosen)

N2 is fully Vulkan-spec-compliant and carries zero risk of overwriting
receiver-side data the app did not author:

- N1 is rejected because it has no flush/invalidate support at all — GPU
  readback breaks and the wire cost is always full-range.
- N3 is rejected because its implicit unmap push corrupts GPU output for
  download-only patterns (see N3's cons above). The forwarder cannot
  tell "host wrote but forgot to flush" from "host never touched this
  region," so any implicit unmap push trades one app bug (forgotten
  flush) for a strictly worse vkfwd bug (silent receiver corruption on a
  spec-compliant readback). N3's `synced_ranges` filter cannot fix this
  — once we drop the unsafe pending push, the bookkeeping collapses to
  a no-op and N3 reduces to N2.
- N4 is rejected for the initial implementation because page protection
  is large and platform-specific work for an optimization. It remains a
  documented follow-up (phase 4) that could re-enable implicit unmap
  flush *safely* by tracking actual host writes rather than guessing.

The accepted cost: an app that writes mapped non-coherent memory without
calling `vkFlushMappedMemoryRanges` before `vkUnmapMemory` loses those
writes on the wire. This is a Vulkan spec violation on the app's side.
vkfwd surfaces it as missing data on the receiver rather than as silent
corruption; the symptom is loud and debuggable. Forgiveness for that
class of app bug requires dirty tracking (N4); attempting to forgive it
without dirty tracking is what makes N3 unsafe.

### Coherent — implementation alternatives

Vulkan does **not** require apps to call flush/invalidate for coherent
memory. GPU-to-CPU and CPU-to-GPU visibility happens at Vulkan visibility
points (fence wait, semaphore wait, queue idle, device idle, queue submit
release), but without explicit app calls to anchor copies, vkfwd has to
decide when to transfer bytes implicitly. This is where the design space
gets uncomfortable.

#### C1 — Mask `HOST_COHERENT` out of `vkGetPhysicalDeviceMemoryProperties`

| | |
|---|---|
| Mechanism | Forwarder hook clears `HOST_COHERENT` on every memory type in the response before generated copy-back. Source app sees only non-coherent memory types. |
| Allocations | `CoherentForwarderAllocation` is never instantiated; everything flows through the non-coherent path. |
| **Pros** | Smallest implementation effort by far. Fully Vulkan-spec compliant from the app's perspective. No new sync hooks, no new wire format, no hazard tracking. |
| **Cons** | Apps that hard-assume coherent availability (some allocators pick coherent if present, without flush/invalidate fallback) may break or pick a wrong memory type. Risk is real but bounded; the failure mode is debuggable. |

#### C2 — Bidirectional coherent strategy (chosen; implemented in sub-stages)

C2 is the chosen coherent strategy. It lands in **sub-stages** rather than
all at once; each sub-stage strictly increases the set of apps that work
spec-correctly, and each ships independently without breaking earlier
sub-stages. The single subclass `CoherentForwarderAllocation` /
`CoherentReceiverAllocation` accumulates behavior across sub-stages.

##### C2.1 — Map/unmap bracketed copies

| | |
|---|---|
| `vkMapMemory` | Allocate staging; copy whole receiver range into staging. |
| `vkUnmapMemory` | Copy whole staging to receiver, then real `vkUnmapMemory`. |
| Sync points | Nothing yet. Persistent maps with GPU writes between `vkMapMemory` and `vkUnmapMemory` see stale source bytes. |
| Correct for | One-shot upload (`map` → CPU writes → `unmap`) and one-shot download (`map` → CPU reads after a prior submit completed → `unmap`). This is the bulk of real coherent usage. |
| Not yet correct for | Persistent coherent maps where the GPU writes after `vkMapMemory` and the CPU reads without remapping. |

##### C2.2 — Skip-copy-on-map flag (covers the "upload-only fast path" need)

| | |
|---|---|
| Adds | A per-allocation or global flag that disables the receiver→source copy at `vkMapMemory`. |
| Selection mechanism | TBD when this sub-stage starts — vkfwd-specific extension, env var, or config knob. |
| When set | Behavior collapses to upload-only: app writes to fresh staging, unmap pushes to receiver, no receiver→source traffic ever. Minimum wire cost. |
| Wrong if | The flag is set on an allocation the GPU actually writes to. Source reads return stale data. App's responsibility to set the flag only when it knows the access pattern. |
| Replaces | The "C5 / upload-only fast path" idea — same outcome, expressed as a flag on C2 rather than a separate strategy class. |

##### C2.3 — Sync-point copies (full Vulkan-spec compliance for persistent maps)

| | |
|---|---|
| Adds | Source → receiver copy of all currently-mapped coherent allocations before each `vkQueueSubmit`. Receiver → source copy after each fence wait / queue idle / device idle / semaphore wait completion (skipped for allocations with the C2.2 flag set). |
| Requires | `vkQueueSubmit`, `vkWaitForFences`, `vkQueueWaitIdle`, semaphore wait/status commands in the generator slice. Today only `vkDeviceWaitIdle` is present. |
| Makes | Persistent coherent maps fully spec-correct under any app behavior. |
| Cost shape | Whole range, every mapped coherent allocation, every sync point. The price of "regardless of cost" compliance. |

##### C2.4 — Diff-based receiver → source transfer (wire optimization)

| | |
|---|---|
| Mechanism | Receiver maintains a per-allocation shadow copy of the bytes it last sent to the forwarder. On the next receiver → source copy (at `vkMapMemory` in C2.1, or at sync points in C2.3), it computes a byte-level diff against the shadow and transmits only changed regions. |
| Applies to | Receiver → source direction only. The source → receiver direction is unchanged. |
| Pros | Big wire savings when the GPU writes a small subset of a large mapped range. No platform-specific code (unlike page protection). |
| Cons | Doubles receiver-side memory cost for each mapped coherent allocation (the shadow copy). Diff computation scales with allocation size; can be page- or chunk-aligned to bound CPU cost. |

##### Summary

| | |
|---|---|
| **Pros** | Spec-compliant in stages. Each sub-stage covers a meaningful class of apps: C2.1 for one-shot patterns, C2.2 adds upload-only as a flag, C2.3 covers persistent coherent maps, C2.4 reduces wire cost in the bidirectional case. Single class, no per-stage type explosion. |
| **Cons** | Up through C2.3, transfers are whole-range (until C2.4). Sync-point copies in C2.3 over-copy because every mapped coherent allocation gets synced regardless of which the GPU actually touched — C3 (below) is the further optimization for that. |

#### C3 — Resource-hazard tracker (layers on top of C2.3)

| | |
|---|---|
| Mechanism | At `vkQueueSubmit`, record which mapped allocations could have been read or written by the submitted command buffers (via bound buffers, images, descriptors). Attach the set to the fence/semaphore. At the corresponding wait/idle, sync only those allocations rather than all currently-mapped coherent allocations. |
| Layered on | C2.3. C3 is the "skip allocations the GPU didn't touch" optimization that C2.3 lacks. |
| **Pros** | Avoids syncing allocations the GPU never wrote. Long-term correct-and-fast path. |
| **Cons** | Large implementation. Requires `vkBindBufferMemory` / `vkBindImageMemory` tracking, descriptor set bind state, command buffer recording state, per-fence dirty sets. Cuts deeper into receiver replay than anything else in this design. Phase 4 territory. |

#### C4 — Page-protection dirty tracking on source side

| | |
|---|---|
| Mechanism | Same as N4 (PROT_NONE staging pages, fault handler) but applied to coherent allocations to narrow the source → receiver copy at sync points to dirty pages only. |
| Layered on | C2.3 (and optionally C3). C4 is the source-side counterpart to C2.4's receiver-side diff — together they minimize both directions. |
| **Pros** | Minimum source → receiver bytes on the wire even for coherent access. |
| **Cons** | Same platform-specific complexity as N4. Optimization layer; doesn't replace C2 or C3, it sharpens the source-side copy. |

### How the alternatives compose

The non-coherent slot is locked to **N2**. The coherent slot is locked to
**C2**, implemented in sub-stages. C3 and C4 are optimizations that layer
on top of C2.3 and can land independently. C1 is documented as a fallback
ergonomic option — clearing `HOST_COHERENT` from the response so apps that
hard-assume coherent availability degrade to non-coherent gracefully — and
is not currently planned for any phase; until C2.1 lands in phase 3a,
coherent allocations get the empty `CoherentForwarderAllocation` placeholder
whose methods return `VK_ERROR_FEATURE_NOT_PRESENT`. N4 remains a
documented follow-up that could re-enable a safe implicit unmap flush on
top of N2 once page-protection dirty tracking is in place.

The factory only cares about which subclass to instantiate for a given
memory type. The strategy class is fixed at allocation time; new behavior
within a strategy (a C2 sub-stage, a flag) lands as additional code in the
same class.

## Execution plan

Each phase commits to specific choices from the alternative tables above
and ships in isolation.

| Phase | Non-coherent | Coherent | Behavior delta |
|---|---|---|---|
| **0 (this work)** | empty placeholder | empty placeholder | Skeleton: framework, factory, registry, hooks. `vkMapMemory` still returns `VK_ERROR_FEATURE_NOT_PRESENT`. |
| **1** | **N2 (map/unmap only)** | empty placeholder | First working end-to-end map/unmap for non-coherent. `NonCoherentForwarderAllocation` ships the map and unmap entry points. N2 does no data transfer at map or unmap (see N2's table row), so the phase-1 wire protocol carries no payload bytes in either direction — apps that write to mapped non-coherent memory do not yet see their writes reach the receiver, and apps that read from mapped memory see uninitialized staging. This is harmless rather than corrupting; phase 2 wires flush/invalidate so writes propagate. Coherent allocations get the empty `CoherentForwarderAllocation` placeholder (the factory still constructs one), whose `map`/`unmap`/`flush`/`invalidate` all return `VK_ERROR_FEATURE_NOT_PRESENT`, so any app that picks a coherent memory type sees that error on the subsequent `vkMapMemory`. Dropping `HOST_COHERENT` from `vkGetPhysicalDeviceMemoryProperties` (C1) is documented as a future option that would let apps that hard-assume coherent availability degrade to non-coherent gracefully; it is not implemented in phase 1. `vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` not yet in the generator slice. |
| **2** | **N2 (flush/invalidate added)** | empty placeholder | `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` ship as real entry points. Flush pushes source→receiver immediately; invalidate pulls receiver→source immediately. With both wired, non-coherent map/unmap is end-to-end Vulkan-spec-compliant for compliant apps. GPU readback works. Apps that forget to flush before unmap lose those writes — surfaced as missing data on the receiver, not corruption. Coherent allocations still get the placeholder `CoherentForwarderAllocation` and error from its methods. |
| **3a** | N2 | **C2.1** (map/unmap bracketed copies) | `CoherentForwarderAllocation` starts doing real work: fetch on map, push on unmap. Handles one-shot upload and one-shot download patterns correctly. Persistent coherent maps with GPU writes after the map call still see stale data. |
| **3b** | N2 | **C2.2** (skip-copy-on-map flag) | Per-allocation or global flag disables the receiver→source copy on map. App opt-in for upload-only workloads. Selection mechanism (extension / env var / config) decided when 3b starts. |
| **3c** | N2 | **C2.3** (sync-point copies) | Source→receiver before `vkQueueSubmit`, receiver→source after fence/queue/device wait/idle and semaphore wait/status. Requires those commands in the generator slice. Persistent coherent maps now fully Vulkan-spec compliant. |
| **3d** | N2 | **C2.4** (diff-based receiver→source) | Receiver maintains a shadow copy of last-sent bytes per mapped allocation; transmits only changed regions on subsequent receiver→source copies. Wire-cost optimization. |
| **4** | **N4** (optional) | **C3** (hazard tracker, optional) and/or **C4** (page protection, optional) | Page-protection dirty tracking on source side narrows source→receiver copies and re-enables a safe implicit unmap flush — apps that forget to flush before unmap get their actually-written pages pushed, without N3's corruption risk. N4 layers on the reserve+commit VM scaffolding already shipped in phase 1, so the new code is the fault handler and dirty-page accounting only — not a rewrite of the staging path. Hazard tracker narrows which allocations participate in C2.3's sync-point copies. |
| **5** | — | — | Broaden tests beyond per-allocation unit tests into full receiver-replay correctness against real Vulkan. |

Each phase 3 sub-stage is independently reviewable; sub-stages 3a-3d may
be batched into one larger landing if the implementation effort is small
enough, but they remain conceptually distinct so that interim stops are
viable. For instance, shipping 3a + 3b without 3c is a valid resting
point — it handles one-shot patterns and upload-only persistent maps, and
documents persistent coherent maps with GPU writes as a known limitation
until 3c lands.

The rest of this document specifies phase 0 in detail and sketches the
wire protocol and algorithms that phase 1+ will fill in.

---

# Phase 0 — Skeleton Framework

## Goal

Reshape the memory-map code so that:

- The forwarder owns one polymorphic `ForwarderAllocation` instance **per
  live `VkDeviceMemory` handle**, dispatched by the manager.
- The receiver owns a mirrored `ReceiverAllocation` instance per handle.
- Each subclass corresponds to one handling strategy (non-coherent,
  coherent). Strategy choice is committed at `vkAllocateMemory` time and
  never changes for that allocation.
- Adding a new strategy is one new pair of files plus one factory branch.
  No edits to the manager, the hooks, the receiver session, or
  `ReplayContext`.

## Non-goals for phase 0

- No new user-visible behavior. `vkMapMemory` still returns
  `VK_ERROR_FEATURE_NOT_PRESENT`. `vkFlushMappedMemoryRanges` /
  `vkInvalidateMappedMemoryRanges` are not added to the generator slice in
  this phase.
- No real staging-buffer allocation. The non-coherent / coherent subclasses
  are empty placeholders that preserve today's error returns.
- No wire-format additions. The phase-1+ wire format below is sketched but
  not implemented in phase 0.
- No receiver-side `MemoryTypeRegistry`. The forwarder caches what it can from
  app-visible property queries; phase 1 adds a custom fallback query for apps
  that allocate memory without first asking vkfwd for memory properties.
- No coherent strategy implementation. C2 is the chosen coherent strategy, but
  phase 0 only commits the framework shape.
- `core/hook/` (the pack/unpack hook layer) is untouched. It is zero
  runtime cost and not in the way; it stays as it is.

## File Layout

```
src/vkfwd/ferry/core/memory_map/
    manager.hpp / manager.cpp                           # MemoryMapForwarder + MemoryMapReceiver facades
    memory_type_registry.hpp / memory_type_registry.cpp # forwarder-side caches keyed by VkDevice / VkPhysicalDevice

    forwarder_allocation.hpp / forwarder_allocation.cpp # abstract base
    forwarder_allocation_factory.hpp / .cpp             # CreationInfo -> std::unique_ptr<ForwarderAllocation>
    forwarder/
        non_coherent_allocation.hpp / .cpp              # phase-0 empty placeholder; real impl in phase 1
        coherent_allocation.hpp / .cpp                  # phase-0 empty placeholder; real impl when phase 3 picks a strategy

    receiver_allocation.hpp / receiver_allocation.cpp   # abstract base
    receiver_allocation_factory.hpp / .cpp
    receiver/
        non_coherent_allocation.hpp / .cpp              # phase-0 empty placeholder
        coherent_allocation.hpp / .cpp                  # phase-0 empty placeholder

    test/                                               # phase-0 tests
        internal-test.cmake
        memory_type_registry_test.cpp
```

New hook files under the forwarder entry-point hook layer:

```
src/vkfwd/ferry/forwarder/hook/
    vkCreateDeviceForwarderHook.hpp
    vkDestroyDeviceForwarderHook.hpp
    vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp
    vkGetPhysicalDevicePropertiesForwarderHook.hpp
```

Existing files removed or moved:

- `src/vkfwd/ferry/core/memory_map_manager.{hpp,cpp}` → split / relocated
  into `core/memory_map/manager.{hpp,cpp}` plus the new per-strategy files.

## ForwarderAllocation

```cpp
namespace vkfwd::memory_map {

class ForwarderAllocation {
public:
    struct CreationInfo {
        VkDevice              device;
        VkDeviceMemory        memory;                  // caller-visible source handle, owned by the app
        VkDeviceSize          allocation_size;
        uint32_t              memory_type_index;
        VkMemoryPropertyFlags property_flags;          // resolved via MemoryTypeRegistry at allocate time
        VkDeviceSize          non_coherent_atom_size;  // from VkPhysicalDeviceLimits
        std::size_t           min_memory_map_alignment;// from VkPhysicalDeviceLimits
    };

    virtual ~ForwarderAllocation() = default;

    // vkMapMemory: resolve VK_WHOLE_SIZE against allocation_size, allocate
    // or reuse staging, optionally fetch initial bytes from the receiver,
    // write a source-process-valid pointer into *ppData.
    virtual VkResult map(VkDeviceSize offset, VkDeviceSize size,
                         VkMemoryMapFlags flags, void ** ppData) = 0;

    // vkUnmapMemory: finalize the staging boundary. Subclasses may attach
    // a payload to the outgoing command stream so the receiver-side unmap
    // endpoint can apply source bytes before the real vkUnmapMemory call.
    virtual void unmap() = 0;

    // vkFlushMappedMemoryRanges: source -> receiver for one range. The
    // manager iterates VkMappedMemoryRange[] and calls this per range
    // belonging to this allocation.
    virtual VkResult flush(VkDeviceSize offset, VkDeviceSize size) = 0;

    // vkInvalidateMappedMemoryRanges: receiver -> source for one range.
    virtual VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) = 0;

    const CreationInfo & info() const { return info_; }

protected:
    explicit ForwarderAllocation(const CreationInfo & info);

private:
    CreationInfo info_;
};

} // namespace vkfwd::memory_map
```

Design decisions baked into the interface:

- `flush` and `invalidate` are on the base from day one. They are not
  Vulkan-app-visible until phase 2 ships the corresponding entry points, but
  fixing the contract now means subclasses are written once. Phase-0
  placeholders return `VK_ERROR_FEATURE_NOT_PRESENT`; this is not reachable
  through any caller path the app can drive.
- Coherent-specific sync hooks (`on_queue_submit`, `on_fence_wait`, etc.)
  are **not** on the base. They are strategy-internal and will be added, if
  needed, when a coherent strategy that requires them is picked. The base
  interface stays tied to Vulkan-app-visible API boundaries.
- `flush` / `invalidate` take a single `(offset, size)` range, not an array.
  The manager iterates `VkMappedMemoryRange[]` and dispatches per range
  because each range targets one `VkDeviceMemory` handle and therefore one
  allocation instance.

## ReceiverAllocation

Mirrors `ForwarderAllocation` with receiver-side signatures. Each method
consumes the relevant slice of the request stream and produces the response
payload, then drives real Vulkan via the dispatch table.

```cpp
class ReceiverAllocation {
public:
    struct CreationInfo {
        VkDevice              device;        // source-visible VkDevice (also the per-context dispatch key)
        VkDeviceMemory        memory;        // source-visible VkDeviceMemory; the MemoryMapReceiver per-handle map uses this same value as its key, matching the handle that arrives in wire payloads
        VkDeviceSize          allocation_size;
        uint32_t              memory_type_index;
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        std::size_t           min_memory_map_alignment;
    };

    // Subclass invariant: the receiver-native VkDevice / VkDeviceMemory that
    // real Vulkan calls require are obtained via ReplayContext's source→
    // receiver handle map at the call site (the same translation that every
    // other receiver endpoint performs). The CreationInfo stores source-
    // visible handles so the per-handle map key and the wire payload agree.
    // The handle-map dependency is documented under "What's Deferred" and is
    // a prerequisite for phase 1.

    virtual ~ReceiverAllocation() = default;

    virtual bool map_endpoint(const CommandStream & request_stream,
                              const Range & request_range,
                              CommandStream & response_stream,
                              ReplayContext & replay_context) = 0;
    virtual bool unmap_endpoint(const CommandStream & request_stream,
                                const Range & request_range,
                                CommandStream & response_stream,
                                ReplayContext & replay_context) = 0;
    virtual bool flush_endpoint(const CommandStream & request_stream,
                                const Range & request_range,
                                CommandStream & response_stream,
                                ReplayContext & replay_context) = 0;
    virtual bool invalidate_endpoint(const CommandStream & request_stream,
                                     const Range & request_range,
                                     CommandStream & response_stream,
                                     ReplayContext & replay_context) = 0;

    const CreationInfo & info() const { return info_; }

protected:
    explicit ReceiverAllocation(const CreationInfo & info);

private:
    CreationInfo info_;
};
```

The receiver-side base intentionally keeps the receiver mapped pointer (if
any) and replay state private to subclasses. Receiver mapped pointers must
never escape to source-process state; the base interface offers no accessor
for them.

## MemoryTypeRegistry

Forwarder-side singleton. Caches per-physical-device data already passing
through forwarder hooks, so allocate-time code can usually resolve
`memoryTypeIndex` → `VkMemoryPropertyFlags` in O(1). The cache is an
optimization and a local source of truth when the application has already
queried properties through vkfwd; it is not a Vulkan call-order requirement.

```cpp
namespace vkfwd::memory_map {

class MemoryTypeRegistry {
public:
    static MemoryTypeRegistry & instance();

    void record_device(VkDevice device, VkPhysicalDevice physical_device);
    void forget_device(VkDevice device);

    void record_memory_properties(VkPhysicalDevice physical_device,
                                  const VkPhysicalDeviceMemoryProperties & properties);
    void record_non_coherent_atom_size(VkPhysicalDevice physical_device,
                                       VkDeviceSize size);
    void record_min_memory_map_alignment(VkPhysicalDevice physical_device,
                                         std::size_t alignment);

    struct Resolved {
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        std::size_t           min_memory_map_alignment;
    };

    // Returns nullopt when the device, the physical device, or the memory
    // type index has not been populated yet. Callers must treat this as
    // "vkfwd cannot classify this allocation from cache yet", not as an
    // application Vulkan-order violation.
    std::optional<Resolved> resolve(VkDevice device,
                                    uint32_t memory_type_index) const;

private:
    MemoryTypeRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<VkDevice, VkPhysicalDevice>                         device_to_physical_;
    std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> memory_properties_;
    std::unordered_map<VkPhysicalDevice, VkDeviceSize>                     non_coherent_atom_;
    std::unordered_map<VkPhysicalDevice, std::size_t>                      min_memory_map_alignment_;
};

} // namespace vkfwd::memory_map
```

The registry intentionally exposes only what `CreationInfo` needs.
Terminology used throughout this document:

- **Classified** — the per-handle entry exists in `MemoryMapForwarder`'s
  `allocations` map. Implies `resolve(...)` succeeded at allocate time and
  the factory picked a subclass. All in-map entries are classified by
  construction.
- **Untracked** — the per-handle entry does not exist (classification
  failed, even after fallback). `MemoryMapForwarder` has no record at
  all. `vkMapMemory` on an untracked handle returns
  `VK_ERROR_FEATURE_NOT_PRESENT` from the manager surface;
  `vkUnmapMemory` is a manager-side no-op (the public entry still
  returns void as the spec requires); `vkFreeMemory` continues to
  forward through the generated path so the receiver actually frees the
  allocation — only the manager's per-handle bookkeeping is unchanged.

The fallback runs at **allocate time only**, inside
`vkAllocateMemoryForwarderHook::after_response_unpack` (the only trigger
site). If `resolve(...)` misses there, the hook issues a synchronous
`manual::CommandId::QueryPhysicalDeviceMemoryInfo` request, caches the
response, and retries `resolve(...)`. Only if the retry also misses does
the allocation become untracked. There is no map-time or use-time fallback
path; once an allocation is recorded, it is classified, and once an
allocation is untracked, it stays untracked for its lifetime. This keeps
the manager surface and the per-allocation strategies free of any
"deferred classification" state.

In every case — fallback success, fallback failure, or untracked outcome —
vkfwd must not fail a successful `vkAllocateMemory` solely because the
forwarder-side cache was incomplete. Until the fallback exists (i.e., in
phases that ship before phase 1 wires up
`QueryPhysicalDeviceMemoryInfo`), missing cache data simply means the
manager skips recording that allocation and any subsequent map attempt
returns `VK_ERROR_FEATURE_NOT_PRESENT`.

The registry lives in `core/memory_map/` rather than `core/` because the
forwarder-side use case is currently memory-map-specific. If a second
subsystem starts needing the same caches, the registry promotes to `core/`
without an interface change.

## Factories

```cpp
class ForwarderAllocationFactory {
public:
    static std::unique_ptr<ForwarderAllocation>
        create(const ForwarderAllocation::CreationInfo & info);
};
```

Branching is committed in phase 0:

```cpp
std::unique_ptr<ForwarderAllocation>
ForwarderAllocationFactory::create(const ForwarderAllocation::CreationInfo & info) {
    const bool host_visible  = (info.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)  != 0;
    const bool host_coherent = (info.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    // Non-mappable memory has no map manager identity. The allocate hook
    // records nothing; any later vkMapMemory on that handle fails at the
    // manager surface (no per-handle entry, so custom_vkMapMemory_entry
    // returns VK_ERROR_FEATURE_NOT_PRESENT) without ever reaching the
    // receiver. The receiver-side driver would also reject the call if
    // it ever arrived, but the manager short-circuit means we never send
    // the chunk.
    if (!host_visible) { return nullptr; }

    if (host_coherent) { return std::make_unique<CoherentForwarderAllocation>(info); }
    return std::make_unique<NonCoherentForwarderAllocation>(info);
}
```

`ReceiverAllocationFactory` has the same shape. Its branching is not
exercised in phase 0; see the next section.

## Receiver factory deferral

The receiver-side files (base class, factory, empty subclasses, per-handle
map inside `MemoryMapReceiver`) all land in phase 0. The factory's
branching path is **not** exercised in phase 0 — the per-handle map stays
empty for the whole phase.

The receiver does not yet have property-flag information at allocate-endpoint
time. Phase 1 uses forwarder-side classification and custom memory-map protocol
state rather than adding a receiver-side `MemoryTypeRegistry`: the forwarder is
responsible for resolving memory strategy, and the manual map/unmap path carries
the state the receiver needs to choose or validate the matching
`ReceiverAllocation`.

Phase 0 receiver-side concrete behavior:

- `vkAllocateMemory_endpoint` and `vkFreeMemory_endpoint` stay generic
  generated endpoints. No generator change in phase 0 to route them through
  the manager. The receiver's per-handle map is never populated.
- Standard generated `vkMapMemory_endpoint` and `vkUnmapMemory_endpoint` remain
  compiled as generated Vulkan coverage but are not the supported memory-map
  path. The full implementation sends `manual::CommandId::MemoryMap` and
  `manual::CommandId::MemoryUnmap` instead. Receiver hooks for the standard
  generated map/unmap commands must fail closed with a clear log message. The
  standard generated map path must never call real receiver `vkMapMemory`,
  because that would create an untracked receiver mapped pointer and could pack
  a receiver-process address into a generated Vulkan response.

Phase 1 required work: add receiver dispatch for `vkfwd::manual::CommandId`,
enable `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS` for `vkMapMemory` /
`vkUnmapMemory`, and wire the receiver per-handle map with the source-to-receiver
handle correspondence needed by the custom protocol.

## Manager Surface

### `MemoryMapForwarder`

Today's `Impl::allocations` is a flat map of POD `AllocationRecord`. It
becomes:

```cpp
std::unordered_map<VkDeviceMemory, std::unique_ptr<ForwarderAllocation>> allocations;
```

The allocation-record methods and custom public entry handlers become thin
dispatch under the same mutex:

| Method | Body |
|---|---|
| `record_allocation(device, memory, property_flags, memory_type_index, allocation_size, non_coherent_atom_size, min_memory_map_alignment)` | Build `CreationInfo`. Call factory. Insert if non-null. |
| `forget_allocation(memory)` | Erase from map. unique_ptr destructor releases any staging owned by the subclass. |
| `custom_vkMapMemory_entry(...)` | Public Vulkan `vkMapMemory` forwarder entry delegates here once `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS` enables the custom path. Look up unique_ptr; call `->map(offset, size, flags, ppData)`. Returns `VK_ERROR_FEATURE_NOT_PRESENT` if the handle is not in the map. |
| `custom_vkUnmapMemory_entry(device, memory)` | Public Vulkan `vkUnmapMemory` forwarder entry delegates here once the custom path is enabled. Look up unique_ptr; call `->unmap()`. |

The `record_allocation` signature changes — it now requires the resolved
`property_flags`, `memory_type_index`, `non_coherent_atom_size`, and
`min_memory_map_alignment`. The `vkAllocateMemoryForwarderHook` is updated to
look these up via `MemoryTypeRegistry::instance().resolve(...)` before calling.

New public methods, added now to lock the manager surface for phase 2 (no
callers in phase 0):

```cpp
VkResult flush_ranges(VkDevice device,
                      uint32_t range_count,
                      const VkMappedMemoryRange * ranges);
VkResult invalidate_ranges(VkDevice device,
                           uint32_t range_count,
                           const VkMappedMemoryRange * ranges);
```

Each iterates the input array, dispatches per `(memory, offset, size)` to
the appropriate per-allocation method, attempts every range, and returns the
first non-success `VkResult` observed. Phase 0 has no entry-point caller for
either.

`test_get_allocation_size` stays. Its body becomes a one-line lookup that
reads `allocation->info().allocation_size`.

### `MemoryMapReceiver`

Same shape change: per-`ReplayContext` map of
`std::unique_ptr<ReceiverAllocation>`. The receiver exposes
`custom_vkMapMemory_endpoint` and `custom_vkUnmapMemory_endpoint` for
`manual::CommandId::MemoryMap` / `manual::CommandId::MemoryUnmap` chunks. These
methods unpack vkfwd's custom memory-map payloads, look up the allocation by
source-visible memory handle, and forward to the matching `ReceiverAllocation`
method.

The receiver also grows `record_allocation` and `forget_allocation` methods.
**None of these are called from generated endpoint code in phase 0.** Phase 1
adds receiver dispatch for the manual command ids and the handle/strategy-tag
state needed to populate the per-handle map. In phase 0 the per-handle map stays
empty, and any transitional generated standard `vkMapMemory` / `vkUnmapMemory`
hook must fail closed rather than creating untracked receiver mappings.

## Hooks

All four new hooks land in the forwarder entry-point hook layer
(`forwarder/hook/`), not the core pack/unpack hook layer. They capture
source-process-only state and need typed `Response` access, both natively
available in the forwarder entry-point hook layer.

| File | Phase | Body |
|---|---|---|
| `vkCreateDeviceForwarderHook.hpp` | `after_response_unpack` | If `response.return_value == VK_SUCCESS && *parameters.pDevice != VK_NULL_HANDLE`, call `MemoryTypeRegistry::instance().record_device(*parameters.pDevice, parameters.physicalDevice)`. |
| `vkDestroyDeviceForwarderHook.hpp` | `after_pack` | `MemoryTypeRegistry::instance().forget_device(parameters.device)`. Done at `after_pack` so a later allocate on the same handle cannot resolve through the doomed device. |
| `vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp` | `after_response_unpack` | If `parameters.pMemoryProperties`, call `record_memory_properties(parameters.physicalDevice, *parameters.pMemoryProperties)`. |
| `vkGetPhysicalDevicePropertiesForwarderHook.hpp` | `after_response_unpack` | If `parameters.pProperties`, call `record_non_coherent_atom_size(parameters.physicalDevice, parameters.pProperties->limits.nonCoherentAtomSize)` and `record_min_memory_map_alignment(parameters.physicalDevice, parameters.pProperties->limits.minMemoryMapAlignment)`. |

The existing `vkAllocateMemoryForwarderHook` body changes. **Phase 0**
ships a single-resolve body — no `QueryPhysicalDeviceMemoryInfo` fallback,
because the manual command id and its receiver dispatch do not exist yet
(see "Out of scope for phase 0" in the implementation plan). Phase 1 adds
the fallback retry. Both bodies share the same "leave untracked on
classification failure, do not fail the allocation" terminal behavior:

```cpp
// Phase-0 body.
static void after_response_unpack(const Parameters & parameters, Response & response) {
    if (response.return_value != VK_SUCCESS || !parameters.pAllocateInfo
        || !response.pMemory || *response.pMemory == VK_NULL_HANDLE) { return; }

    auto & registry = MemoryTypeRegistry::instance();
    auto   resolved = registry.resolve(parameters.device,
                                       parameters.pAllocateInfo->memoryTypeIndex);
    if (!resolved) {
        // The app allocated without first calling
        // vkGetPhysicalDeviceMemoryProperties / vkGetPhysicalDeviceProperties
        // through the layer, so the registry has no entry for this memory type.
        // Phase 0 has no QueryPhysicalDeviceMemoryInfo fallback yet; the handle
        // becomes untracked. The receiver-side allocation succeeded, so the
        // source must not fail vkAllocateMemory. vkMapMemory on this handle
        // returns VK_ERROR_FEATURE_NOT_PRESENT from the manager surface;
        // vkUnmapMemory does nothing manager-side; vkFreeMemory still forwards
        // through the generated path so the receiver actually frees, only
        // manager bookkeeping is unchanged.
        VKFWD_LOG_ERROR("vkfwd: untracked allocation, "
                        "device={} memoryTypeIndex={}", parameters.device,
                        parameters.pAllocateInfo->memoryTypeIndex);
        return;
    }

    MemoryMapForwarder::instance().record_allocation(
        parameters.device, *response.pMemory,
        resolved->property_flags,
        parameters.pAllocateInfo->memoryTypeIndex,
        parameters.pAllocateInfo->allocationSize,
        resolved->non_coherent_atom_size,
        resolved->min_memory_map_alignment);
}
```

**Phase 1** wraps a fallback retry around the same body:

```cpp
// Phase-1 body. Diff from phase 0: one retry through
// QueryPhysicalDeviceMemoryInfo between the two resolve() calls.
static void after_response_unpack(const Parameters & parameters, Response & response) {
    // ... same early-out as phase 0 ...
    auto resolved = registry.resolve(parameters.device,
                                     parameters.pAllocateInfo->memoryTypeIndex);
    if (!resolved) {
        // Vulkan does not require apps to call vkGetPhysicalDeviceMemoryProperties
        // / vkGetPhysicalDeviceProperties before vkAllocateMemory, so the cache
        // may legitimately be empty on the first allocation. Phase 1 ships a
        // synchronous QueryPhysicalDeviceMemoryInfo fallback to fill the cache
        // from the receiver, then retries the resolve. This is the only trigger
        // site for QueryPhysicalDeviceMemoryInfo — classification is an
        // allocate-time invariant: any allocation that reaches
        // MemoryMapForwarder's per-handle map is fully classified by the time
        // it is recorded.
        request_memory_info_fallback(parameters.device);
        resolved = registry.resolve(parameters.device,
                                    parameters.pAllocateInfo->memoryTypeIndex);
    }
    // ... same untracked-on-still-unresolved branch and same record_allocation
    //     call as phase 0 ...
}
```

`request_memory_info_fallback` is a thin helper on the forwarder side: look
up the device's `VkPhysicalDevice` via `MemoryTypeRegistry`, build a
`QueryPhysicalDeviceMemoryInfoRequest`, send it through `Forwarder::flush()`
(synchronous), and on a successful response feed `record_memory_properties`,
`record_non_coherent_atom_size`, and `record_min_memory_map_alignment` back
into the registry. If the device-to-physical association is missing or the
wire request fails, the helper is a no-op and the second `resolve` will miss
again — that is the path that exercises the untracked-allocation case below.

The other three existing memory-map hooks (`vkFreeMemoryForwarderHook`,
`vkMapMemoryForwarderHook`, `vkUnmapMemoryForwarderHook`) keep their
current shape; they already call into the manager and don't care which
subclass backs a given handle.

No generator change is required for these four hook headers. The forwarder
entry-point template already emits the `__has_include("hook/<api>ForwarderHook.hpp")`
block for every generated command.

## Phase 0 placeholder behavior

`NonCoherentForwarderAllocation` and `CoherentForwarderAllocation` ship in
phase 0 as empty placeholders — same shape, same return values:

```cpp
class NonCoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void ** ppData) override {
        if (ppData) { *ppData = nullptr; }
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map not yet implemented");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    void     unmap()                                 override {}
    VkResult flush(VkDeviceSize, VkDeviceSize)       override { return VK_ERROR_FEATURE_NOT_PRESENT; }
    VkResult invalidate(VkDeviceSize, VkDeviceSize)  override { return VK_ERROR_FEATURE_NOT_PRESENT; }
};
```

`CoherentForwarderAllocation` is byte-for-byte the same in phase 0 — same
error returns, same log line tagged with its own class name. Receiver-side
placeholders mirror.

This preserves today's user-visible behavior (`vkMapMemory` returns
`VK_ERROR_FEATURE_NOT_PRESENT`) regardless of which memory type the app
picks.

---

# Phase 1+ — Wire Protocol

The memory-map protocol is hand-written and uses vkfwd-owned command ids
declared in `src/vkfwd/ferry/core/custom_command.hpp`:

```cpp
namespace vkfwd::manual {
enum class CommandId : std::uint32_t {
    MemoryMap,
    MemoryUnmap,
    QueryPhysicalDeviceMemoryInfo,
};
}
```

These ids share `CommandChunkHeader::command_id` with generated Vulkan command
ids, but they are a separate protocol surface. A receiver must dispatch manual
ids before the generated Vulkan endpoint switch. Public application calls still
enter `vkMapMemory` / `vkUnmapMemory`; only the wire command id and payload are
custom.

### Command-id namespace invariant

Generated and manual command ids share one 32-bit space in
`CommandChunkHeader::command_id`. Collision protection is structural, not
conventional:

- `core/command_id_range.hpp` defines
  `constexpr std::uint32_t kReservedCommandIdBase = 0xFFFE0000u`. The
  upper region `[kReservedCommandIdBase, 2^32)` is reserved for manual
  ids; the lower region `[1, kReservedCommandIdBase)` belongs to the
  generator. Zero is a "not yet set" sentinel.
- The generator's `stable_command_id` reduces its SHA-256-derived hash
  modulo `kReservedCommandIdBase` and rewrites any zero result to 1, so
  no generated id can ever land in the reserved range.
- Every per-command generated header emits
  `static_assert(static_cast<std::uint32_t>(::vkfwd::generated::CommandId::<Name>) < ::vkfwd::kReservedCommandIdBase, ...)`.
  Any future change to the hash function that broke the invariant would
  fail to build.
- `core/custom_command.hpp` emits the complementary
  `static_assert(... >= kReservedCommandIdBase, ...)` for every manual
  id, catching the inverse mistake.

Together these make collision impossible at build time. Adding a new
manual id requires only picking a value in the reserved range and
adding it to `custom_command.hpp`; nothing else needs to coordinate.

The manager protocol has its own revision:

```cpp
inline constexpr std::uint32_t kMemoryMapManagerRevision = 1;
```

The revision is stored in every manual memory-map request/response. A mismatch
is a protocol error. Increment it for any layout, staging-transfer, alignment,
or cache-maintenance semantic change.

## QueryPhysicalDeviceMemoryInfo

The forwarder uses app-visible property queries to prime `MemoryTypeRegistry`,
but applications are not required to call those queries before
`vkAllocateMemory`. When allocation classification misses the cache and
`vkCreateDevice` has recorded a `VkDevice -> VkPhysicalDevice` association, the
forwarder sends this synchronous manual query:

```cpp
struct QueryPhysicalDeviceMemoryInfoRequest {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
};

struct QueryPhysicalDeviceMemoryInfoResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value = VK_SUCCESS;
    VkPhysicalDeviceMemoryProperties memory_properties {};
    VkDeviceSize non_coherent_atom_size = 0;
    std::uint64_t min_memory_map_alignment = 0;
};
```

The receiver answers by calling real
`vkGetPhysicalDeviceMemoryProperties(physical_device, ...)` and
`vkGetPhysicalDeviceProperties(physical_device, ...)`, then copying
`limits.nonCoherentAtomSize` and `limits.minMemoryMapAlignment` into the
response. The forwarder caches the response and retries the registry
resolve. If the device-to-physical mapping is unknown (the device was not
seen through our `vkCreateDevice` hook) or the manual query itself fails
(transport error, receiver non-success), vkfwd logs the missing state and
leaves the allocation **untracked** — it must not fail a successful
`vkAllocateMemory` solely because forwarder-side classification could not
complete. The subsequent surface behavior of an untracked handle is
described in the `MemoryTypeRegistry` section above.

This is the only place the `QueryPhysicalDeviceMemoryInfo` command is sent.
It is not a generic memory-info query; it exists exclusively as the
allocate-time fallback for the registry.

## Source-Side Staging: VM Reserve + Commit

The forwarder allocates source-side staging using OS virtual-memory
primitives in a two-phase reserve/commit pattern, not `aligned_alloc`. The
reservation covers the full `allocation_size`; only the
`[mapped_offset, mapped_offset + effective_size)` sub-range is committed and
made readable/writable. The app-visible pointer is `reservation_base +
mapped_offset`.

This design is what real Vulkan drivers do for `vkMapMemory`, and it is the
structural way to satisfy the spec contract that `*ppData − offset` be
aligned to `minMemoryMapAlignment`:

- `reservation_base` is page-aligned by OS guarantee. The Vulkan spec
  imposes a 64-byte floor on `minMemoryMapAlignment` but does not cap it
  from above; in practice no shipping driver returns more than 256, while
  page size is at least 4 KiB on every supported platform (16 KiB on
  Apple Silicon and modern ARM64 Android; Windows `VirtualAlloc` returns
  reservations on its 64 KiB allocation granularity, larger still). To
  avoid silently returning a misaligned `*ppData` if a future driver ever
  reports a larger `minMemoryMapAlignment` than the host page size, the
  manager asserts `min_memory_map_alignment <= host_page_size` at
  allocate-record time. The assertion compiles in every build — it is a
  correctness invariant, not a debug check.
- `*ppData − offset == reservation_base`, which is page-aligned by
  construction — regardless of whether `offset` is itself aligned.
- Allocation-relative offsets become directly usable as staging offsets
  (`reservation_base[K]` ≡ allocation byte `K`), so wire-format and copy
  code carry no per-allocation base-subtraction math.
- Sub-window maps of large allocations (suballocator patterns) cost only
  the committed sub-range in physical memory; the reservation is virtual
  address space, which is effectively free on 64-bit hosts.

### Platform primitives

| OS | Reserve | Commit | Release |
|---|---|---|---|
| Linux | `mmap(NULL, allocation_size, PROT_NONE, MAP_PRIVATE \| MAP_ANONYMOUS \| MAP_NORESERVE, -1, 0)` | `mprotect(base + page_floor(offset), page_ceil(end) - page_floor(offset), PROT_READ \| PROT_WRITE)` | `munmap(base, allocation_size)` |
| Windows | `VirtualAlloc(NULL, allocation_size, MEM_RESERVE, PAGE_NOACCESS)` | `VirtualAlloc(base + page_floor(offset), page_ceil(end) - page_floor(offset), MEM_COMMIT, PAGE_READWRITE)` | `VirtualFree(base, 0, MEM_RELEASE)` |
| macOS | `mmap(NULL, allocation_size, PROT_NONE, MAP_PRIVATE \| MAP_ANONYMOUS, -1, 0)` | `mprotect(base + page_floor(offset), page_ceil(end) - page_floor(offset), PROT_READ \| PROT_WRITE)` | `munmap(base, allocation_size)` |
| Android | `mmap(NULL, allocation_size, PROT_NONE, MAP_PRIVATE \| MAP_ANONYMOUS \| MAP_NORESERVE, -1, 0)` | `mprotect(base + page_floor(offset), page_ceil(end) - page_floor(offset), PROT_READ \| PROT_WRITE)` | `munmap(base, allocation_size)` |

A thin internal abstraction (`vm_reserve`, `vm_commit`, `vm_release`) lives
in a single header and hides the platform `#ifdef`. The wrapper is the only
place these syscalls are named.

### Platform quirks

- **Windows: 64 KiB allocation granularity.** `VirtualAlloc` rounds the
  base address of a `MEM_RESERVE` to 64 KiB boundaries, not the 4 KiB page
  size. Commits within the reservation are still page-aligned. The reserved
  base is therefore 64 KiB-aligned on Windows — well above any
  `minMemoryMapAlignment` we will ever encounter, so the pointer-alignment
  contract is satisfied with margin.
- **16 KiB pages on Apple Silicon and modern ARM64 Android.** Page-aligned
  commit rounding can extend the committed region by up to 16 KiB on either
  side of the mapped sub-range, instead of 4 KiB. Still negligible against
  any realistic allocation; the wire path must never transmit those
  slivers.
- **Linux/Android strict overcommit (`vm.overcommit_memory == 2`).** A
  `PROT_NONE` `MAP_ANONYMOUS` reservation can charge against the commit
  limit under strict accounting. `MAP_NORESERVE` makes the reservation
  charge-free and is a no-op under the default permissive mode, so we set
  it unconditionally on Linux and Android.
- **`mprotect` / commit failure under memory pressure.** `mprotect` can
  return `ENOMEM` and `VirtualAlloc(MEM_COMMIT)` can return `NULL` if the
  OS cannot back the requested pages. The forwarder treats either as
  `VK_ERROR_OUT_OF_HOST_MEMORY` from `vkMapMemory`, releases the
  reservation, and returns to the application without recording any
  staging state.

### What goes on the wire

The committed sub-range covers `[page_floor(mapped_offset), page_ceil(mapped_end))`,
which may include sliver bytes outside the mapped range due to page
rounding. The mapped-range bounds, **not** the committed-region bounds,
gate every wire transfer and every flush/invalidate range validation. The
slivers exist only so the OS commit call has page-aligned arguments; they
are never read, written, or transmitted by vkfwd.

### Synergy with N4

The reserve+commit scaffolding is exactly the layer N4 (page-protection
dirty tracking) needs. N4 ships as an addition on top of N2: it installs a
`SIGSEGV` / Windows VEH handler, transitions committed pages between
`PROT_NONE` and `PROT_READ | PROT_WRITE` to record dirty state, and
populates the unmap-time pending payload from the dirty page set. The
reservation, commit, release, and page-rounding logic are unchanged. This
is why N4 was sized in the execution plan as an optional optimization
rather than a rewrite of the non-coherent path.

## MemoryMap Request And Response

The public `vkMapMemory` forwarder entry delegates to
`MemoryMapForwarder::custom_vkMapMemory_entry(...)`. That method packs a manual
`manual::CommandId::MemoryMap` chunk, not generated `CommandId::MapMemory`:

```cpp
struct MemoryMapRequest {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0; // may be VK_WHOLE_SIZE
    VkMemoryMapFlags flags = 0;
};

struct MemoryMapResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value = VK_SUCCESS;
    VkDeviceSize effective_size = 0;
};
```

No pointer crosses the wire. The receiver calls real `vkMapMemory`, stores the
receiver mapped pointer privately on the matching `ReceiverAllocation`, resolves
`VK_WHOLE_SIZE` against the receiver allocation record, and returns only the
driver result plus `effective_size`. The forwarder acquires source-side
staging via the reserve+commit pattern described above — reservation covers
the full `allocation_size`, commit covers the mapped sub-range — records
`mapped_allocation_range = [offset, offset + effective_size)`, and writes
`reservation_base + offset` into the application's `ppData`. This satisfies
the Vulkan `*ppData − offset` alignment contract by construction.

Both `vkMapMemory` and `vkUnmapMemory` are synchronous in the custom path.
`vkMapMemory` must wait for `effective_size`; `vkUnmapMemory` must wait until
the receiver has consumed staged bytes before source staging can be freed.

## MemoryUnmap Request

The public `vkUnmapMemory` forwarder entry delegates to
`MemoryMapForwarder::custom_vkUnmapMemory_entry(...)`. That method packs a
manual `manual::CommandId::MemoryUnmap` chunk:

```cpp
struct MemoryUnmapRequestHeader {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    VkDevice device = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize mapped_offset = 0;
    VkDeviceSize mapped_size = 0;
    std::uint32_t range_count = 0;
    std::uint32_t reserved = 0;
};

struct MemoryTransferRange {
    VkDeviceSize offset = 0; // allocation-relative
    VkDeviceSize size = 0;
    std::uint64_t payload_offset = 0; // from start of command chunk
};
```

The header is followed by `range_count` range records and then the staged byte
payloads. All public Vulkan ranges are allocation-relative. With VM-backed
staging, the reservation base corresponds to allocation byte 0, so copy code
addresses staging directly by the allocation-relative offset — no
base-subtraction step:

```text
staging_byte = reservation_base + allocation_relative_offset
```

Under N2 the unmap chunk carries `range_count = 0` and no payload bytes — all
source → receiver transfers happen synchronously inside `vkFlushMappedMemoryRanges`
(phase 2), never at unmap. The `MemoryTransferRange` layout and the header's
`range_count` slot are retained for symmetry with the flush wire format and
for future N4, where dirty-page bytes the host wrote without an explicit
flush would be carried in the unmap chunk. `VK_WHOLE_SIZE` is resolved when
the map is accepted; flush/invalidate ranges resolve it against the
allocation and then intersect with the active mapped range.

On receiver unmap, `NonCoherentReceiverAllocation` copies each payload range
into the receiver mapped pointer (zero payload ranges under phase-1/2 N2),
then calls real `vkFlushMappedMemoryRanges` for any copied
allocation-relative ranges expanded as required by `nonCoherentAtomSize`,
then calls real `vkUnmapMemory`. The receiver-side flush step is a no-op
when no payload ranges were carried; it remains in the unmap path for the
N4 case where ranges may be present.

## vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges (phase 2)

Phase 2 adds manual command ids or payload variants for explicit flush and
invalidate. Flush copies source staging to receiver memory and then performs
real receiver-side `vkFlushMappedMemoryRanges` with atom-size-aligned ranges.
Invalidate performs real receiver-side `vkInvalidateMappedMemoryRanges` before
copying receiver bytes back to source staging. Under N2 the forwarder does
not track which ranges have been synced — each flush/invalidate call is a
self-contained transfer, and unmap carries no payload. Per-allocation
synced-range bookkeeping is deferred to N4, where dirty-page detection makes
it usable safely.

---

# Phase 1+ — Algorithm Sketches

These describe what `NonCoherentForwarderAllocation::map()` and friends
will do in phase 1. The dummy placeholders today do none of this.

## NonCoherentForwarderAllocation::map(offset, size, flags, ppData)

Classification is an **allocate-time invariant**: any allocation that
reaches this method is fully classified by construction (the allocate hook
ran the `QueryPhysicalDeviceMemoryInfo` fallback if the registry missed,
and only recorded the allocation once `resolve(...)` succeeded). Untracked
handles never reach here — `MemoryMapForwarder::custom_vkMapMemory_entry`
returns `VK_ERROR_FEATURE_NOT_PRESENT` for them at the manager surface.
So `map()` carries no classification branch.

Source-side staging is reserved and committed **before** the receiver is
asked to map. If either VM call fails, the source returns
`VK_ERROR_OUT_OF_HOST_MEMORY` without ever sending `MemoryMap`, so there
is no receiver-side mapping to clean up. The map request only goes on the
wire after staging is live locally.

```
1. effective_size = (size == VK_WHOLE_SIZE) ? (allocation_size - offset) : size.
   (allocation_size came from the registered allocation record; the
    receiver re-resolves VK_WHOLE_SIZE independently and the validation in
    step 7 catches any disagreement.)
2. reservation_base = vm_reserve(allocation_size).
   On failure: return VK_ERROR_OUT_OF_HOST_MEMORY.
3. commit_begin = page_floor(offset).
   commit_end   = page_ceil(offset + effective_size).
   vm_commit(reservation_base + commit_begin, commit_end - commit_begin).
   On failure: vm_release(reservation_base, allocation_size);
               return VK_ERROR_OUT_OF_HOST_MEMORY.
4. Build and append a manual MemoryMap request chunk with
   command_id=manual::CommandId::MemoryMap.
5. response_stream = forwarder.flush().
6. Read MemoryMapResponse from response_stream at offset 0.
7. Validate the response:
   a. manager_revision must match. A mismatch is a session-fatal
      incompatibility (see "Invariants Summary"); release the
      reservation, tear the session down, and return VK_ERROR_UNKNOWN
      to the caller.
   b. The receiver's resolved effective_size must match the source-side
      value from step 1. A mismatch is a per-call protocol error;
      release the reservation and return VK_ERROR_UNKNOWN. The session
      stays up — a single divergent allocation does not invalidate
      everything else mid-flight.
8. If return_value != VK_SUCCESS:
   vm_release(reservation_base, allocation_size);
   return return_value.
9. Store reservation_base, mapped_offset=offset, effective_size, flags, and
   mapped_allocation_range on this allocation.
10. *ppData = reservation_base + offset.
11. Return VK_SUCCESS.
```

Step 5 is synchronous: `flush()` blocks until the receiver has processed
the map request and returned the response. Steps 7–8 still own the
reservation when they fail, so the early-return paths release it; from
step 9 onward the allocation's stored state owns the reservation and its
destructor (via `forget_allocation`) handles release.

Step 10 satisfies the Vulkan contract `*ppData − offset` aligned to
`minMemoryMapAlignment` by construction, because `reservation_base` is
page-aligned by OS guarantee and page size dominates `minMemoryMapAlignment`
on every supported platform (enforced by the allocate-time assertion
under "Source-Side Staging: VM Reserve + Commit").

### Why this ordering

A naive ordering ("send `MemoryMap` first, then reserve staging") leaks
the receiver mapping if `vm_reserve` or `vm_commit` fails after the
receiver already executed real `vkMapMemory`. Two ways to avoid that:

- **Local-first (chosen):** reserve+commit before the wire round-trip.
  Local failures never reach the receiver. The cost is one extra VM
  reservation in the case where the receiver itself rejects the map
  (step 8 releases it), but that path is exceptional.
- **Compensating unmap:** send `MemoryMap`, attempt local staging, and
  on local failure append a compensating `MemoryUnmap` chunk before
  returning the error. Adds a second wire round-trip on every local
  failure and a new "did this `MemoryUnmap` ever have a matching
  `MemoryMap`?" edge case for the receiver.

Local-first is simpler and removes a class of receiver-cleanup bugs at
no steady-state cost.

## NonCoherentForwarderAllocation::unmap()

```
1. Read reservation_base, allocation_size, mapped_offset, and effective_size
   from this allocation's stored state.
2. Build a manual MemoryUnmap request:
   a. append a CommandChunkHeader with command_id=manual::CommandId::MemoryUnmap.
   b. append MemoryUnmapRequestHeader{device, memory, mapped_offset,
      mapped_size=effective_size, range_count=0}.
   c. (no MemoryTransferRange entries and no payload bytes under N2.)
   d. finalize the command chunk.
3. Forwarder::instance().flush()  — synchronous; receiver has unmapped before return.
4. vm_release(reservation_base, allocation_size); clear stored state.
```

Under N2 the unmap chunk is header-only — any host writes the app cared
about were already delivered by an earlier `vkFlushMappedMemoryRanges`. An
app that wrote staging without calling flush before unmap loses those
writes; this is a Vulkan-spec violation on the app's side and is surfaced
as missing data on the receiver, not as corruption. N4 would later extend
this step to attach dirty-page payload bytes for pages the host wrote
since the last flush; the wire layout and the VM scaffolding both already
accommodate that without further changes.

Step 4 is safe because `flush()` is synchronous; the receiver has finished
processing the unmap before this thread returns from `flush()`, so no
reader of the staging pages remains when the reservation is released.

The unmap forwarder hook does not inspect the response stream (it is
empty). Logging an error if `flush()` reports transport failure is
acceptable; there is no meaningful VkResult to return since `vkUnmapMemory`
is void.

## NonCoherentReceiverAllocation::map_endpoint(...)

```
1. Unpack MemoryMapRequest from the manual command chunk.
2. Look up receiver_device and receiver_memory from the receiver handle
   maps (these arrive with phase 1's handle-map work).
3. Call receiver driver vkMapMemory(receiver_device, receiver_memory,
      offset, size, flags, &receiver_ptr).
   - If size == VK_WHOLE_SIZE, the driver resolves effective_size against
     the actual allocation; the receiver also records this from the
     vkAllocateMemory replay record.
4. Build MemoryMapResponse{ manager_revision, return_value,
      effective_size=resolved }.
5. Append response bytes to response_stream.
6. If return_value == VK_SUCCESS, store receiver_ptr and effective_size on
   this allocation's private state. receiver_ptr never leaves the receiver.
7. Return true.
```

On failure at step 3, `effective_size` in the response is 0 and
`return_value` is the driver error code. No receiver state is created.

## NonCoherentReceiverAllocation::unmap_endpoint(...)

```
1. Unpack MemoryUnmapRequestHeader and MemoryTransferRange[] from the manual
   command chunk.
2. Validate manager_revision and that every range is inside the active mapped
   allocation range. On mismatch: log, return false.
3. Copy each payload into receiver_ptr at
   receiver_offset = range.offset - mapped_offset.
4. For non-coherent memory, call receiver driver vkFlushMappedMemoryRanges for
   the copied allocation-relative ranges expanded to nonCoherentAtomSize.
5. Call receiver driver vkUnmapMemory(receiver_device, receiver_memory).
6. Clear this allocation's private state.
7. Return true.
```

Step 3 writes bytes through `receiver_ptr` directly into device memory. The
receiver-side Vulkan flush in step 4 is what makes those host writes visible to
the destination GPU for non-coherent memory.

## Coherent strategy

See [Coherent — implementation alternatives](#coherent--implementation-alternatives)
in the Overall Design section above for the chosen C2 strategy, its four
sub-stages (C2.1–C2.4), and the C3 / C4 optimizations that may layer on
top. Phase 0 ships `CoherentForwarderAllocation` /
`CoherentReceiverAllocation` as empty files; phases 1 and 2 leave them as
empty placeholders whose `map`/`unmap`/`flush`/`invalidate` methods all
return `VK_ERROR_FEATURE_NOT_PRESENT`, so `vkMapMemory` on a coherent
allocation errors at that placeholder rather than ever reaching real
Vulkan. Phase 3 lands the C2 sub-stages.

---

# Test Plan

Tests land **only after each piece of real behavior lands**. Phase 0 does
not introduce tests for empty placeholder behavior. The plan below
describes which tests cover which piece, with the phase they're introduced
in.

## Phase 0 — tests that ship

1. **`MemoryTypeRegistry` unit tests** —
   `core/memory_map/test/memory_type_registry_test.cpp`:
   - record_device then resolve returns the recorded physical device.
   - record_memory_properties then resolve returns the matching property
     flags for a known memoryTypeIndex.
   - record_non_coherent_atom_size flows through resolve.
   - record_min_memory_map_alignment flows through resolve.
   - resolve before either of the precondition records returns nullopt.
   - forget_device makes subsequent resolve return nullopt for that device.

2. **Memory-type registry hooks integration test** — under
   `forwarder/test/`. Uses the existing pack/unpack test transport (same
   pattern as `vkAllocateFreeMemory_test.cpp`) to call the generated entry
   points for `vkCreateDevice`,
   `vkGetPhysicalDeviceMemoryProperties`,
   `vkGetPhysicalDeviceProperties`, synthesize a success response, and
   assert that `MemoryTypeRegistry::instance().resolve(...)` returns the
   expected values afterwards. Then call `vkDestroyDevice_entry` and
   assert resolve falls back to nullopt.

3. **Existing tests continue to pass after small setup adjustments:**
   - `vkAllocateFreeMemory_test.cpp` currently calls `vkAllocateMemory_entry`
     against a fresh `MemoryMapForwarder` without populating
     `MemoryTypeRegistry`. After the refactor, the allocate hook resolves
     property flags via the registry, so the test must first prime the
     registry with `record_device(test_device, test_physical_device)`
     and the corresponding `record_memory_properties` /
     `record_non_coherent_atom_size` /
     `record_min_memory_map_alignment` for the memory type index it uses.
     The assertion on `test_get_allocation_size` then continues to hold.
   - `vkMapMemory_test.cpp` does not allocate first; it just maps a
     synthetic handle. To keep its assertion
     (`VK_ERROR_FEATURE_NOT_PRESENT`) meaningful,
     `MemoryMapForwarder::custom_vkMapMemory_entry` returns
     `VK_ERROR_FEATURE_NOT_PRESENT` when the handle is not in the
     per-allocation map, matching the placeholder subclasses' return
     code. The test can keep its current shape; it now exercises the
     "no record" path explicitly rather than the unconditional
     placeholder.

## Tests NOT shipped in phase 0

- No factory branching test. The factory's branch into one of two empty
  placeholders has no meaningful behavior to assert yet.
- No `ForwarderAllocation` or `ReceiverAllocation` subclass tests. Empty
  placeholders should not be locked in with regression tests that would
  just have to be deleted in phase 1.
- No tests for the new `flush_ranges` / `invalidate_ranges` manager
  methods. Phase 0 has no caller; phase 2 will add the entry points and
  the tests together.

## Phase 1 — adds with non-coherent N2 map/unmap (coherent stays an empty placeholder)

- `NonCoherentForwarderAllocation` unit tests:
  - **map success path:** the call first reserves `allocation_size` VA
    and commits the page-aligned span covering the mapped sub-range,
    *then* sends `manual::CommandId::MemoryMap`, then on a successful
    response returns `reservation_base + offset` as `*ppData` and
    records the active mapped allocation range. The test transport
    records call order and asserts staging is live before the wire
    chunk is dispatched. Also asserts
    `(*ppData − offset) % minMemoryMapAlignment == 0` for a range of
    offsets including unaligned ones.
  - **unmap success path:** sends `manual::CommandId::MemoryUnmap` with
    `range_count = 0` and no payload bytes (N2), then releases the
    reservation only after the synchronous flush returns.
  - **local-failure ordering (regression for the staging-leak bug):**
    inject a `vm_reserve` failure and assert that **no** `MemoryMap`
    chunk reached the wire and that the call returns
    `VK_ERROR_OUT_OF_HOST_MEMORY`. Repeat for `vm_commit` failure with
    the additional assertion that the reservation was released. The
    receiver must remain in its pre-call state.
  - **receiver-rejection cleanup:** the receiver returns a non-success
    `MemoryMapResponse` after the source already reserved+committed.
    Assert the source releases the reservation and propagates the
    receiver's `return_value` to the caller — no staging is retained on
    a receiver-rejected map.
  - **effective_size mismatch:** synthesize a `MemoryMapResponse` whose
    `effective_size` disagrees with the source-resolved value. Assert
    `VK_ERROR_UNKNOWN` is returned and staging is released.
  - **re-map after unmap:** map → unmap → map on the same handle does
    not assert and re-acquires staging cleanly.
- VM primitive wrapper tests: `vm_reserve` returns a page-aligned base or a
  failure marker; `vm_commit` over a sub-range succeeds and yields readable
  and writable bytes; `vm_release` returns the entire reservation. Smoke
  test runs on whichever host the build targets — Linux, macOS, Windows,
  Android — and exercises one reservation across each platform's primitive.
- `NonCoherentReceiverAllocation` unit tests: custom map calls real
  receiver-side `vkMapMemory(receiver_device, receiver_memory, offset,
  size, flags, &receiver_ptr)` and stores `receiver_ptr` in private
  state; custom unmap consumes a header-only `MemoryUnmap` chunk
  (`range_count == 0`, no payload) and calls real `vkUnmapMemory`
  without any intervening `vkFlushMappedMemoryRanges` call (phase 1 N2
  has no flushed ranges to publish). Phase 2 adds the per-range payload
  copy + receiver-side flush tests when flush/invalidate land.
- Manual command dispatch test: receiver routes `vkfwd::manual::CommandId`
  values to `MemoryMapReceiver` before the generated Vulkan endpoint switch.
- Memory-info fallback success path: allocation classification cache miss
  inside `vkAllocateMemoryForwarderHook::after_response_unpack` sends
  `manual::CommandId::QueryPhysicalDeviceMemoryInfo`, the (test) receiver
  responds with valid memory properties / `nonCoherentAtomSize` /
  `minMemoryMapAlignment`, the registry is populated, the retry resolves
  successfully, and `MemoryMapForwarder` records the allocation. Verify the
  handle is in the per-allocation map (`test_get_allocation_size` returns
  the expected size) and that a subsequent `vkMapMemory` proceeds normally.
- **Untracked-allocation path** (regression test for the fallback-also-fails
  case): build a scenario where classification cannot complete — app called
  `vkCreateDevice` through the layer so the device→physical association
  exists, but never called `vkGetPhysicalDeviceMemoryProperties` /
  `vkGetPhysicalDeviceProperties`, and the test transport's
  `QueryPhysicalDeviceMemoryInfo` endpoint returns a non-success
  `return_value`. Call `vkAllocateMemory_entry` with a synthesized success
  response. Assert:
  1. `vkAllocateMemory_entry` returns `VK_SUCCESS` — vkfwd must not fail
     the app's allocation because forwarder-side classification failed.
  2. `test_get_allocation_size(handle) == 0` — the handle is not in
     `MemoryMapForwarder`'s per-allocation map (the documented
     "untracked" signal).
  3. A subsequent `vkMapMemory_entry` on the handle returns
     `VK_ERROR_FEATURE_NOT_PRESENT`.
  4. A subsequent `vkUnmapMemory_entry` on the handle is a deterministic
     no-op (no crash, no wire traffic).
  5. A subsequent `vkFreeMemory_entry` succeeds and the manager
     bookkeeping is unchanged (still no record).
  6. An error log was emitted at allocate time naming the device handle
     and `memoryTypeIndex` (so the operator can diagnose the missed
     cache prime).
- Factory branching test (now meaningful): non-coherent memory type goes
  to `NonCoherentForwarderAllocation`; non-host-visible returns `nullptr`;
  coherent memory type goes to `CoherentForwarderAllocation` (still an
  empty placeholder in phase 1, whose `map`/`unmap`/`flush`/`invalidate`
  all return `VK_ERROR_FEATURE_NOT_PRESENT`). No C1 mask is in effect, so
  apps that pick a coherent type see that error from the placeholder.
- End-to-end loopback (phase 1 baseline, no flush yet): `vkAllocateMemory`
  → `vkMapMemory` → CPU writes → `vkUnmapMemory` → assert the receiver-side
  memory **does not** observe the source writes (no flush call was made;
  N2 deliberately drops them). The "writes reach receiver" loopback test
  moves to phase 2, where it becomes `vkAllocateMemory` → `vkMapMemory` →
  CPU writes → `vkFlushMappedMemoryRanges` → `vkUnmapMemory` → assert
  receiver-side memory has the written bytes.
- Mapping record is released after unmap (re-map on the same handle does
  not assert).
- Per-handle isolation: with two `VkDeviceMemory` handles mapped at
  once, asserting against each handle's source staging shows only its
  own writes — no cross-talk. (This is a structural assertion; phase-1
  N2 has no byte transfer, so the test does not assert receiver-side
  contents. The "writes reach receiver per-handle" assertion lands in
  phase 2 alongside flush coverage.)
- Unmap of unknown handle is deterministic — no crash; error log
  acceptable.
- Revision mismatch on the custom map response is a session-fatal
  error: the session is torn down and the per-call VkResult is
  `VK_ERROR_UNKNOWN`. Because the map algorithm reserves+commits
  staging before sending the wire request, the rejection path must
  explicitly release the reservation (the "effective_size mismatch"
  unit test above exercises the per-call variant of the same
  release-on-validation-fail cleanup).

## Phase 2 — adds with N2 (flush/invalidate)

- Per-range flush/invalidate unit tests on
  `NonCoherentForwarderAllocation`: write → flush(range) → bytes appear
  in receiver memory; receiver writes → invalidate(range) → bytes appear
  in source staging.
- N2 unmap drops unflushed host writes (negative test): app writes to
  staging without calling flush → unmap → assert receiver-side memory
  is unchanged from its pre-map contents and that no payload bytes were
  carried in the unmap chunk. This documents and protects the N2 trade
  so future "helpful" implicit-push regressions are caught.
- Readback safety (regression test for the N3 corruption case): app maps
  → invalidates a sub-range → reads it → unmaps without writing anywhere
  → assert receiver-side memory **outside** the invalidated sub-range is
  byte-identical to its pre-map contents (i.e., no garbage was pushed
  from uninitialized source staging).
- `nonCoherentAtomSize` alignment is honored on every flush/invalidate
  range pair.
- Entry-point tests for `vkFlushMappedMemoryRanges` and
  `vkInvalidateMappedMemoryRanges` once those commands are added to the
  generator slice.
- Manager `flush_ranges` / `invalidate_ranges` end-to-end coverage.
- Multiple concurrent mappings on different handles each carry their
  own flushed bytes to the matching receiver-side memory — no
  cross-talk on the wire, no aliasing in the receiver per-handle map.
  (Promoted from phase-1's isolation-only check now that flush actually
  transfers bytes.)

## Phase 3 — coherent strategy C2 sub-stages

Phase 3 ships `CoherentForwarderAllocation` / `CoherentReceiverAllocation`
in sub-stages. Each sub-stage adds its own tests.

### Phase 3a — C2.1 map/unmap bracketed copies

- `CoherentForwarderAllocation::map` fetches the whole receiver range
  into source staging; CPU can read GPU-written bytes that existed
  before `vkMapMemory`.
- `CoherentForwarderAllocation::unmap` pushes whole staging to
  receiver; CPU writes during the map are visible to subsequent GPU
  reads.
- End-to-end loopback: app maps coherent memory, reads pre-existing
  receiver bytes, writes new bytes, unmaps, GPU sees the new bytes.
- Factory branching test is unchanged in shape (coherent memory type
  still goes to `CoherentForwarderAllocation`), but the placeholder
  no-op assertion on the constructed allocation is replaced with
  assertions against the real C2.1 behavior — fetch on map, push on
  unmap — once the method bodies land.

### Phase 3b — C2.2 skip-copy-on-map flag

- When the flag is set on an allocation, `map` does NOT fetch
  receiver bytes into staging; staging starts undefined.
- When the flag is unset, behavior is identical to C2.1.
- Selection-mechanism tests (vary depending on whatever opt-in
  mechanism the sub-stage picks).

### Phase 3c — C2.3 sync-point copies

- Source→receiver copy of all currently-mapped coherent allocations
  fires before `vkQueueSubmit`.
- Receiver→source copy of all currently-mapped coherent allocations
  (minus those with the C2.2 flag) fires after fence wait, queue
  idle, device idle, and semaphore wait/status completion.
- Persistent-map test: app maps coherent memory, GPU writes to it via
  a submitted command buffer, app reads the mapped pointer after a
  fence wait — sees the new bytes.
- Per-sync-point tests for each newly-added Vulkan command in the
  generator slice (`vkQueueSubmit`, `vkWaitForFences`,
  `vkQueueWaitIdle`, etc.).

### Phase 3d — C2.4 diff-based receiver→source

- After two consecutive receiver→source copies on the same allocation
  where the receiver bytes are mostly unchanged, only the changed
  regions are transmitted.
- Wire-cost regression test: synthesize a coherent allocation where
  the GPU writes one page out of N, verify the wire transmits only
  that page on the second sync.
- Correctness test: bytes the receiver kept in its shadow copy are
  correctly carried forward when not included in the diff.

---

# Decisions Made During Design

Recorded here so future readers do not re-litigate them:

- **`flush` and `invalidate` are on the `ForwarderAllocation` base from
  day one**, even though phase 0 has no caller. Locking the interface
  contract prevents churn when phase 1 implements one subclass and phase
  2 wires the entry points.
- **`flush_ranges` and `invalidate_ranges` exist on `MemoryMapForwarder`
  from day one**, also without callers, for the same reason.
- **No dedicated `Dummy*Allocation` class.** The phase-0 placeholders
  live on `NonCoherentForwarderAllocation` /
  `CoherentForwarderAllocation` (and their receiver counterparts).
  Filling them out is the body of phase 1 / the coherent-strategy
  phase; the file paths and class names never change.
- **Receiver-side per-handle map exists in phase 0 but stays empty.** No
  generated standard Vulkan map/unmap endpoint delegates through
  `MemoryMapReceiver`. Phase 1 populates the map through receiver state needed
  by the custom `vkfwd::manual::CommandId` path.
- **Standard generated map/unmap receiver endpoints fail closed.** Hook files
  for `vkMapMemory` and `vkUnmapMemory` replace those generated endpoints with
  clear error logs. They are obsolete once the public forwarder entry points use
  the manual memory-map protocol and must not be used as a fallback.
- **New hooks land in `forwarder/hook/`, not `core/hook/`.** The four
  registry-feeding hooks need source-process-only state and typed
  `Response` access, both natively available in the forwarder
  entry-point hook layer.
- **`MemoryTypeRegistry` lives in `core/memory_map/`, not `core/`.**
  Promotes if a second subsystem grows the same caching need; no
  interface change required to relocate.

# Open Questions

1. **Coherent strategy sub-stage details.** C2 is chosen, but phase 3 still
   decides the exact selection mechanism for the C2.2 upload-only flag and the
   command coverage/order for C2.3 sync-point copies.

# What's Deferred

- Real per-strategy implementations (phase 1 / phase 3).
- Wire-format extensions for flush/invalidate ranges (phase 2).
- Coherent-specific sync hooks on the interface — added when whichever
  coherent strategy needs them is picked.
- Receiver-side classification from an independent `MemoryTypeRegistry` is not
  planned. The forwarder owns classification, uses
  `manual::CommandId::QueryPhysicalDeviceMemoryInfo` when the app did not prime
  the cache, and sends the custom map/unmap state the receiver needs.
- Adding `vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges`
  to the generator's `TARGET_COMMANDS`. Lands with phase 2.
- Source-to-receiver handle translation for `VkDevice` / `VkDeviceMemory`
  on the receiver side. The phase-1 algorithm sketches assume those maps
  exist; they are a separate handle-map milestone tracked in
  `doc/todo.md` and are a **prerequisite for phase 1 of memory-map
  management**. Phase 1 cannot ship without them: the receiver-side
  `ReceiverAllocation` subclasses obtain the receiver-native `VkDevice`
  and `VkDeviceMemory` via this map at every real-Vulkan call site, and
  phase-0 ships the API surface in anticipation of that translation.
  Phase 0 itself does not need handle translation because its receiver
  endpoints are stubs.
- Wire-protocol distinction between intermediate remap and final unmap.
  C2.3's sync-point copies already handle long-lived "persistent" coherent
  maps (a `VkDeviceMemory` remaining mapped across multiple submit/wait
  cycles) correctly under the current map/unmap wire protocol — the
  receiver-side `vkMapMemory` just stays in effect across the cycles, and
  C2.3 carries the bytes that need to flow. A future optimization could
  add a "persistent remap" vs. "final unmap" distinction so the receiver
  knows when to keep cached state alive across app-driven map/unmap
  cycles; that would require a protocol-revision bump. This is a
  wire-cost optimization, not a correctness prerequisite, and is
  independent of the C2.3 work in phase 3c.
- `VK_EXT_map_memory_placed` (caller-supplied mapped address).

# Invariants Summary

- `MemoryMapForwarder` is a singleton shared across all source threads.
  It is the only permitted creator and owner of source-side staging
  buffers for mapped Vulkan memory.
- Staging buffers are owned by their `ForwarderAllocation` subclass and
  released by the subclass destructor when `forget_allocation` removes
  the per-handle entry. No other code may free or reallocate them.
- Receiver mapped pointers are private to `ReceiverAllocation`
  subclasses. They must never be serialized, exposed on the manager's
  public surface, or returned to the forwarder.
- `flush()` is always synchronous for both map and unmap. The forwarder
  never relies on future delivery.
- Manager revision mismatch between forwarder and receiver is a hard
  incompatibility. The session must not continue after a revision
  mismatch.
- The generated `pack_parameters` / `unpack_parameters` functions for the
  four memory-map commands must not be modified. They remain the source
  of truth for the Vulkan parameter serialization layer. Manager wire
  records sit alongside generated parameter chunks, not inside them.
- Allocation-extent tracking in `MemoryMapForwarder` is bookkeeping for
  range resolution (e.g., `VK_WHOLE_SIZE`); it is not a handle map and
  does not translate source-to-receiver Vulkan object identity.
