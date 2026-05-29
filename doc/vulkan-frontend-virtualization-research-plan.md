# Vulkan Frontend Virtualization Research Plan

This document resets the architectural context after the initial API-forwarding
design work. The working hypothesis is that `vkfwd` should not optimize toward
one remote procedure call per Vulkan API call. That path is useful as a
correctness scaffold, but it fights Vulkan's execution model: a well-written
application records work on the CPU, submits batches to queues, and observes the
GPU only at explicit synchronization, query, memory, and presentation
boundaries.

The performance-oriented question is therefore different:

> Can `vkfwd` behave more like a Vulkan frontend that completes most API calls
> locally while streaming object state, command-buffer contents, memory updates,
> synchronization dependencies, and queue submissions to a remote backend?

The research plan below is meant to answer that question with enough detail to
choose what to reuse, what to copy conceptually, and what to avoid.

## Design Premise

Traditional API forwarding treats each intercepted call as an independent
request whose return value and output parameters are resolved by the receiver
before the application continues. That model is simple to reason about, but the
worst calls for `vkfwd` are exactly the calls that appear in hot loops or frame
pacing paths while still exposing return values or caller-visible output.

The better target is a split frontend/backend model:

- The source-side layer owns Vulkan-facing identities, local dispatch behavior,
  handle virtualization, cacheable query results, command-buffer recording
  state, and source-visible completion policy.
- The stream carries replayable semantic work, not necessarily the original
  call boundary. A command-buffer recording sequence may become one compact
  command-stream payload, and object creation may be split into local identity
  allocation plus asynchronous receiver realization.
- The receiver owns real Vulkan handles and executes work at natural replay
  points: object realization, memory import/update, command-buffer materialize,
  queue submit, waits, queries, acquire/present, and teardown.
- Round trips are reserved for true synchronization points, unavoidable
  source-visible output, error policy that cannot be predicted safely, and
  platform/WSI operations that require receiver truth.

This does not discard the existing endpoint, generated metadata, or pack/unpack
work. Instead, the endpoint becomes the source-visible completion boundary. For
some commands it may synchronously execute a remote operation; for many others
it should complete locally after committing work to a durable asynchronous
stream.

## Research Goals

- Identify existing Vulkan implementations with frontend/backend separation,
  capture/replay machinery, or mature generated dispatch/metadata that `vkfwd`
  can reuse or imitate.
- Determine whether any candidate can be integrated directly without adopting
  its whole runtime, build system, license burden, or platform assumptions.
- Classify Vulkan commands by frontend completion mode:
  `local-immediate`, `deferred-stream`, `batched-submit`, `cached-query`,
  `required-round-trip`, and `unsupported-explicit`.
- Define the minimum source-side frontend state needed to avoid per-call
  round trips while preserving Vulkan-visible behavior.
- Produce a concrete next implementation path for `vkfwd`: likely a hybrid that
  keeps API forwarding for correctness tests while growing a frontend command
  stream for high-frequency paths.

## Non-Goals

- Do not commit to a dependency before reading the implementation code and
  proving its integration shape.
- Do not attempt full Vulkan replay architecture in one step.
- Do not confuse trace capture with live remote execution. File replay systems
  often solve ownership and decoding problems but not source-visible completion.
- Do not assume receiver and source handles, pointers, memory addresses,
  descriptor bytes, or WSI objects can be reused across process or machine
  boundaries.

## Candidate Sweep

### Mesa Venus, venus-protocol, and virglrenderer

Why it matters:

- Venus is the closest known precedent for Vulkan command serialization as a
  guest/frontend to host/backend architecture. Mesa describes it as a Virtio-GPU
  protocol for Vulkan command serialization, with protocol/codegen in
  `venus-protocol` and the renderer in `virglrenderer`.
- Its guest driver has to solve many of the same hard boundaries: handle
  virtualization, command serialization, memory mapping, host feature exposure,
  queue submission, and synchronization.
- Its limitations are just as important as its design. Mesa's documentation
  explicitly calls out implementation-defined behavior around `vkMapMemory`,
  which is a warning sign for any remote frontend design.

Research questions:

- What does Venus complete locally versus send to the host immediately?
- How are guest handle IDs allocated and mapped to host handles?
- How does the protocol encode command buffers, object creation, descriptor
  updates, memory requirements, queue submissions, fences, semaphores, and
  timeline semaphores?
- Which return/output commands are virtualized, cached, or forced synchronous?
- How much of the codegen/protocol machinery can be studied or reused without
  adopting Virtio-GPU, Mesa's driver architecture, or virglrenderer wholesale?
- How does Venus handle WSI and presentation, and does that model apply to a
  non-VM remote-rendering process?

Artifacts to inspect:

- Mesa Venus driver source, especially guest-side object models, command
  encoding, dispatch, memory mapping, and synchronization.
- `venus-protocol` XML/codegen and generated protocol commands.
- `virglrenderer` Venus renderer command decode and host Vulkan invocation.
- vtest path, because it may be the smallest local test harness for exercising
  Venus without a full VM.

Expected leverage:

- High conceptual leverage for command stream shape, object IDs, and sync
  boundaries.
- Medium direct code leverage unless the protocol can be isolated cleanly.
- High risk around build-system size, Mesa internals, Linux/virtio assumptions,
  and memory mapping semantics.

### GFXReconstruct

Why it matters:

- `vkfwd` already vendors GFXReconstruct under `src/third_party/gfxreconstruct`.
  Upstream provides a Vulkan capture layer, replay tool, generated encode/decode
  code, object-info tables, handle mapping, memory tracking, resource dumping,
  and replay overrides.
- It is the best local source for mature Vulkan serialization and replay
  engineering, especially `pNext` ownership, array decoding, handle remapping,
  pipeline and shader replay policy, mapped memory tracking, and generated
  command processors.
- It is file-capture oriented, not live frontend virtualization. That makes it
  a powerful substrate/reference, but not a complete remote execution answer.

Research questions:

- Which generated encoder/decoder pieces can be reused inside `vkfwd_core`
  without adopting the capture-file container?
- Can GFXReconstruct's handle ID scheme, `ObjectInfoTable`, pointer decoders,
  and struct decoders become a bridge for early receiver replay?
- How does it decide when replay must override captured calls instead of
  blindly invoking the same Vulkan command?
- How does its memory tracking behave for persistent maps, page guards,
  coherent/non-coherent memory, and flush/invalidate ranges?
- What would be required to turn its capture stream into a live stream with
  source-visible completion rather than offline replay?

Artifacts to inspect:

- `framework/encode/vulkan_capture_manager.*`
- `framework/decode/vulkan_replay_consumer_base.*`
- `framework/generated/generated_vulkan_*`
- `framework/decode/*object_info*`, handle mapping helpers, and allocator code.
- `layer/layer_vulkan_entry.cpp`
- Existing `vkfwd` generated code, to decide whether to reuse GFXR generators or
  continue a smaller generator that borrows ideas.

Expected leverage:

- High direct leverage for serialization/replay patterns and local tests.
- Medium direct code leverage, because GFXReconstruct is large and designed for
  capture files.
- Low confidence as the final live protocol without significant adaptation.

### Mesa Vulkan Runtime and Utility Code

Why it matters:

- Mesa's Vulkan runtime and utility code provide common driver-facing concepts:
  base object structs, generated dispatch tables, entrypoint lookup, command
  pools, graphics state, render passes, and shared helpers used by multiple
  Mesa Vulkan drivers.
- Mesa's dispatch utility is relevant even if we do not import Mesa. The docs
  describe generated dispatch-table loaders that can populate instance,
  physical-device, and device dispatch tables from `vkGet*ProcAddr`.

Research questions:

- Which Mesa Vulkan runtime pieces are separable utilities versus deeply tied
  to Mesa driver internals?
- Can its dispatch-generation approach inform `vkfwd`'s generated layer
  entrypoints and local frontend tables?
- Are object base classes and command-buffer helpers useful as references for
  `vkfwd`'s local handle/object model?
- What license and build implications would direct code reuse introduce?

Expected leverage:

- High conceptual leverage for generated dispatch and object modeling.
- Low-to-medium direct code leverage unless specific utility files can be
  imported cleanly.

### Vulkan Loader and Validation Layers

Why it matters:

- The Vulkan loader defines the dispatch-table and layer-chain invariants that
  `vkfwd` must preserve. Its documentation states that dispatchable objects
  contain a pointer to a dispatch table and that the loader builds instance and
  device call chains during `vkCreateInstance` and `vkCreateDevice`.
- Validation layers are mature generated interception layers with extensive
  object and state tracking, but they validate usage rather than virtualize a
  remote Vulkan device.

Research questions:

- Are `vkfwd`'s explicit-layer entrypoints and dispatch lookup rules aligned
  with current loader requirements?
- Which validation-layer generator patterns are useful for broad command
  coverage, extension gating, dispatch-level classification, and object state?
- Can validation-style state tracking help decide which commands can complete
  locally without receiver truth?

Expected leverage:

- High leverage for correctness of loader/layer mechanics.
- Medium leverage for generated metadata and object-state taxonomy.
- Low direct leverage for streaming protocol and receiver execution.

### ANGLE, DXVK, VKD3D-Proton, Zink, MoltenVK

Why they matter:

- These projects are not Vulkan frontends for Vulkan applications; they are API
  translators or compatibility layers that emit Vulkan. They still have strong
  lessons in state shadowing, batching, descriptor/pipeline management, shader
  translation boundaries, and asynchronous work scheduling.
- ANGLE's Vulkan backend is a mature frontend/backend separation for GLES over
  Vulkan. DXVK and VKD3D-Proton are production-grade examples of minimizing
  CPU overhead while translating higher-level API semantics to Vulkan.
- Zink and MoltenVK show different tradeoffs for implementing one graphics API
  over another while preserving application-visible behavior.

Research questions:

- How do these projects batch state changes and avoid redundant Vulkan work?
- Which descriptor, pipeline cache, shader cache, and command-buffer reuse
  strategies could apply to a Vulkan frontend stream?
- Which parts are irrelevant because they solve translation from non-Vulkan
  semantics rather than Vulkan virtualization?

Expected leverage:

- Medium conceptual leverage for batching and cache design.
- Low direct code leverage for Vulkan API forwarding.

### gfxstream and Android/Crosvm Graphics Virtualization

Why it matters:

- gfxstream is another mature graphics virtualization family, especially in
  Android emulator and crosvm contexts. It is worth comparing with Venus for
  protocol generation, host renderer organization, cross-domain display, and
  transport choices.

Research questions:

- Does gfxstream's Vulkan path expose a reusable protocol or generator outside
  Android emulator assumptions?
- How does it handle shared memory, external memory, synchronization, and
  presentation compared with Venus?
- Is it more appropriate as a conceptual comparison than a direct dependency?

Expected leverage:

- Medium conceptual leverage.
- Unknown direct leverage until source layout and license/build constraints are
  reviewed.

## Command Completion Taxonomy

The sweep should classify each command family by the source-visible completion
contract, not only by Vulkan category.

| Completion mode | Meaning | Likely examples |
| --- | --- | --- |
| `local-immediate` | Source frontend can return without receiver work because the result is locally allocated, cached, or deterministic under negotiated capabilities. | Local handle ID allocation, cached properties, command-buffer state mutation. |
| `deferred-stream` | Source frontend records owned payloads and sends them asynchronously; receiver realizes later before dependent work. | Most `vkCmd*` commands, descriptor writes, pipeline barriers, image layout commands. |
| `batched-submit` | Work is accumulated locally and materialized on receiver at queue submission or an explicit flush boundary. | Command-buffer end/submit paths, queue submit batches. |
| `cached-query` | First receiver truth may be synchronous, then subsequent calls use a source-side cache keyed by all relevant inputs and `pNext` shape. | Physical-device properties/features, format support, memory requirements. |
| `required-round-trip` | The application cannot continue correctly until receiver-visible progress or output is known. | Fence waits/status, query results, mapped memory readback, acquire/present, allocation failure truth where prediction is unsafe. |
| `unsupported-explicit` | The command is recognized but deliberately rejected or falls back to local passthrough only with a visible limitation. | Platform interop, unsupported WSI, rare vendor extensions. |

This taxonomy should be generated into metadata eventually. The first manual
pass should focus on the high-frequency return/output groups already listed in
`doc/vulkan-return-output-api-groups.md`.

## Minimum Frontend State To Define

Before implementing the frontend stream, define the minimum state the
source-side layer must own:

- Source handle allocator and source-to-receiver identity table. Source handles
  must be stable and immediately usable by the application; receiver handles are
  separate and may not exist yet for deferred objects.
- Dispatchable-object wrappers that preserve Vulkan loader-chain invariants and
  carry `vkfwd` object identity plus next-layer dispatch tables.
- Capability snapshot and query cache keyed by physical device, enabled
  extensions, feature/property structure shape, input parameters, and relevant
  `pNext` chains.
- Command pool and command buffer recording state, including reset/begin/end
  validity, command-buffer payload ownership, and externally synchronized access
  assumptions.
- Descriptor, pipeline layout, render pass/dynamic rendering, framebuffer,
  image view, sampler, buffer, image, memory, and acceleration-structure object
  metadata needed to serialize dependent commands without asking the receiver.
- Host memory mirror policy for mapped memory, staging uploads, flush/invalidate
  ranges, coherent memory, persistent maps, and receiver readback.
- Queue timeline state sufficient to order submissions, waits, semaphore
  signals, fence signals, and present operations.
- Error policy for deferred realization failures. A local frontend can return
  success early only if the eventual receiver failure has a defined reporting or
  termination path.

## Work Plan

### Phase 0: Baseline Constraints

Output: `doc/vulkan-frontend-virtualization-findings.md` skeleton plus a
spreadsheet or generated markdown table of command completion modes.

Tasks:

- Re-read current `vkfwd` docs and align terminology: endpoint, replay,
  source-visible completion, trace-only, generated metadata, handle mapping.
- Freeze the initial comparison criteria:
  correctness surface, hot-path overhead, codegen fit, memory model, WSI model,
  transport independence, license/build cost, and testability.
- Select a minimal benchmark workload: triangle, descriptor-heavy draw loop,
  mapped upload loop, fence/query polling loop, and acquire/present loop.

### Phase 1: Local GFXReconstruct Deep Dive

Output: a findings section with reusable components, adaptation risks, and
specific files/classes to prototype against.

Tasks:

- Trace capture entrypoint flow from the Vulkan layer into
  `VulkanCaptureManager`.
- Trace replay flow from a decoded block into `VulkanReplayConsumerBase`.
- Document how GFXReconstruct owns copied parameter data, `pNext` chains,
  output handles, memory snapshots, page-guard tracking, and replay object info.
- Compare GFXReconstruct's generated metadata with `vkfwd`'s current generator.
- Prototype one narrow adapter decision on paper: either reuse its generated
  decoders for receiver replay, or keep `vkfwd` generation but borrow its
  object-info and memory-tracking patterns.

Acceptance criteria:

- We can state exactly which GFXReconstruct pieces are candidates for direct
  reuse, which are reference-only, and which conflict with live streaming.

### Phase 2: Venus Deep Dive

Output: a frontend/backend architecture comparison and a list of Venus protocol
ideas to copy or reject.

Tasks:

- Read Mesa guest Venus object model and command encoding path.
- Read venus-protocol command definitions and generated encoder/decoder shape.
- Read virglrenderer Venus renderer decode and host Vulkan invocation path.
- Identify every forced synchronous command family in Venus.
- Study memory mapping and synchronization handling carefully; document where
  Venus relies on virtio/VM assumptions that `vkfwd` may not have.
- Run or at least build the vtest path if practical, then capture logs for
  startup, object creation, command-buffer recording, queue submit, map/unmap,
  and present.

Acceptance criteria:

- We can decide whether Venus is a protocol to adapt, a set of concepts to
  imitate, or too coupled to virtio/Mesa to use directly.

### Phase 3: Mesa Runtime, Loader, and Validation-Layer Sweep

Output: dispatch/object-state design notes for `vkfwd`.

Tasks:

- Compare Mesa generated dispatch tables with `vkfwd`'s planned forwarder
  generated entrypoints.
- Verify loader/layer chain behavior against current Khronos loader docs.
- Inspect validation-layer generated command tables and object-state tracking
  for reusable classification patterns.
- Decide whether `vkfwd` should remain an explicit layer or grow an ICD-like
  frontend mode for experiments.

Acceptance criteria:

- `vkfwd` has a clear loader/dispatch plan that does not violate dispatchable
  object invariants and does not confuse layer passthrough with frontend-owned
  dispatch.

### Phase 4: Translator Project Sweep

Output: batching/cache lessons, not dependency recommendations.

Tasks:

- Inspect ANGLE Vulkan backend state manager and command submission flow.
- Inspect DXVK and VKD3D-Proton descriptor/pipeline/cache/submission paths.
- Inspect Zink and MoltenVK only for high-level state shadowing and backend
  boundary patterns.
- Extract techniques relevant to Vulkan-over-stream: descriptor batching,
  pipeline cache behavior, async pipeline compilation, command-buffer reuse,
  and redundant state suppression.

Acceptance criteria:

- We have a short list of implementation techniques worth copying without
  mistaking translators for Vulkan virtualization systems.

### Phase 5: Architecture Decision

Output: a concrete `vkfwd` architecture decision record.

Decision options:

1. Continue generated API forwarding first, then optimize.
2. Build a `vkfwd`-native Vulkan frontend stream while borrowing concepts from
   Venus and GFXReconstruct.
3. Adapt Venus protocol or virglrenderer directly.
4. Adapt GFXReconstruct encode/decode/replay as the live stream substrate.
5. Support two modes: correctness-oriented API forwarding and
   performance-oriented frontend virtualization.

Likely recommendation to validate:

- Use a hybrid path. Keep generated API forwarding as the conformance and
  debugging scaffold, but make the performance path a `vkfwd`-native frontend
  stream. Borrow heavily from Venus for live frontend/backend semantics and from
  GFXReconstruct for payload ownership, generated decode, object tables, and
  replay override discipline.

Acceptance criteria:

- The decision names what to implement next, what to defer, and what evidence
  would invalidate the decision.

## Immediate Prototype After The Sweep

Prototype a tiny live frontend path for one narrow workload:

1. `vkCreateInstance`, physical-device enumeration, and `vkCreateDevice` still
   use synchronous endpoint completion.
2. Command-buffer recording commands complete locally into an owned
   frontend-side command stream.
3. `vkQueueSubmit` flushes the command buffer payload to the receiver and waits
   only as required by the submitted fence/semaphore policy.
4. One mapped upload path is implemented with an explicit copied memory update,
   not raw pointer forwarding.
5. One cached property query proves cache-key handling with a `pNext` chain.

The prototype should deliberately exclude WSI at first. Presentation introduces
platform policy and frame pacing, so it should be the next focused prototype
after headless queue submission works.

## Source Links

- Mesa Venus documentation:
  <https://docs.mesa3d.org/drivers/venus.html>
- Mesa Vulkan dispatch documentation:
  <https://docs.mesa3d.org/vulkan/dispatch.html>
- Khronos Vulkan loader interface architecture:
  <https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderInterfaceArchitecture.md>
- GFXReconstruct upstream repository:
  <https://github.com/LunarG/gfxreconstruct>
- ANGLE upstream repository:
  <https://github.com/google/angle>
