# Mesa Vulkan Frontend Report

This report analyzes Mesa's Vulkan frontend organization with emphasis on the
Venus virtio Vulkan driver and the shared Mesa Vulkan runtime. It is written
for `vkfwd`'s architecture work: what can be borrowed conceptually, what may be
reusable as code, and what is too coupled to Mesa/Virtio-GPU assumptions.

Inspected source:

- Mesa checkout: `/home/chenli/dev/god/_research/mesa`
- Mesa commit: `2a60e9e7697` on `main`
- Primary directories:
  - `src/virtio/vulkan`
  - `src/virtio/venus-protocol`
  - `src/vulkan/runtime`
  - `src/vulkan/util`
  - `src/vulkan/wsi`

Public references:

- Mesa Venus docs: <https://docs.mesa3d.org/drivers/venus.html>
- Mesa source repository docs: <https://docs.mesa3d.org/repository.html>
- Mesa source tree docs: <https://docs.mesa3d.org/sourcetree.html>
- Mesa Vulkan runtime docs: <https://docs.mesa3d.org/vulkan/index.html>
- Mesa dispatch docs: <https://docs.mesa3d.org/vulkan/dispatch.html>
- Mesa base object docs: <https://docs.mesa3d.org/vulkan/base-objs.html>
- Mesa command pool docs: <https://docs.mesa3d.org/vulkan/command-pools.html>
- Mesa source repository: <https://gitlab.freedesktop.org/mesa/mesa>

## Executive Summary

Mesa Venus is the closest project in the sweep to the `vkfwd` performance
hypothesis. It is not traditional per-call RPC. It is a real Vulkan ICD-like
guest driver that owns local Vulkan handles, local object metadata, dispatch
tables, command-buffer recording, caches, feedback slots, memory mapping
policy, WSI integration, and a command-stream transport to a renderer.

The core pattern is:

1. The app calls Venus as a Vulkan ICD.
2. Venus creates local Mesa runtime objects and returns local handles.
3. Venus assigns each object a `vn_object_id`.
4. Generated venus-protocol encoders serialize commands by object ID rather
   than by raw local pointer.
5. Commands are sent through a ring/shared-memory command stream to
   virglrenderer or vtest.
6. Most command-buffer recording commands only append bytes to a local encoder.
7. `vkEndCommandBuffer`, queue submission, selected sync/query paths, and
   memory/WSI paths materialize or synchronize with the renderer.

For `vkfwd`, the main lesson is that the frontend must own enough local state
to return quickly and safely. Venus does this aggressively: buffers/images can
be created asynchronously after cache hits, command buffers are recorded into
owned local command streams, queue submits are usually asynchronous, fences and
semaphores use feedback slots to avoid repeated synchronous polling, and many
physical-device queries are cached.

The main warning is also clear: Venus leans on Virtio-GPU, host-visible blob
resources, dma-buf, syncobj/sync-file behavior, and implementation-defined
`vkMapMemory` assumptions. Those transport and memory assumptions should not be
copied blindly into `vkfwd`.

## High-Level Source Organization

Mesa's upstream repository docs confirm that the canonical read-only source is
`https://gitlab.freedesktop.org/mesa/mesa.git`, with `main` carrying current
development. The official source-tree docs place common Vulkan support under
`src/vulkan`, hardware Vulkan drivers under driver-specific directories such as
`src/amd/vulkan`, `src/intel/vulkan`, and `src/freedreno/vulkan`, and Gallium
frontends under `src/gallium/frontends`. Venus follows the same Mesa pattern:
its reusable Vulkan substrate comes from `src/vulkan`, while the actual remote
Vulkan frontend/guest driver lives in `src/virtio/vulkan`.

For `vkfwd`, the official tree map is useful because it separates four things
that are easy to blur together:

- `src/vulkan`: common Vulkan driver/frontend infrastructure, not one driver.
- `src/*/vulkan`: hardware Vulkan ICD implementations such as RADV, ANV, and
  Turnip.
- `src/gallium/frontends`: API frontends built on Gallium drivers, including
  Lavapipe as a Vulkan software frontend.
- `src/gallium/drivers/zink`: an OpenGL-on-Vulkan Gallium driver, useful for
  state translation ideas but not a Vulkan frontend in the same sense as Venus.

That classification is important for leverage analysis. Venus is the primary
remote-execution frontend model. `src/vulkan/runtime` is the reusable local
object/dispatch substrate. Hardware drivers are useful for understanding how
Mesa Vulkan frontends own local API state before emitting backend work. Zink is
more relevant to GL-to-Vulkan state translation than to Vulkan-to-remote-Vulkan
virtualization.

### Venus Guest Driver

Directory: `src/virtio/vulkan`

Important files:

| File | Role |
| --- | --- |
| `vn_icd.c`, `vn_icd.h` | Loader/ICD exported entrypoints such as `vk_icdGetInstanceProcAddr` and loader interface negotiation. |
| `vn_common.h`, `vn_common.c` | Shared Venus object bases, object ID allocation, environment/debug flags, TLS rings, cached temporary storage, wait/poll relax logic. |
| `vn_instance.c`, `vn_instance.h` | Instance creation, renderer creation, CPU ring creation, protocol version negotiation, physical-device enumeration cache. |
| `vn_physical_device.c`, `vn_physical_device.h` | Renderer capability discovery, physical-device features/properties caches, extension filtering, memory/format/image-format query policy. |
| `vn_device.c`, `vn_device.h` | Logical device creation, queue initialization, feedback pool setup, memory-requirement caches, device-level dispatch. |
| `vn_command_buffer.c`, `vn_command_buffer.h` | Local command-buffer encoder, command pool/buffer lifecycle, command batching, query feedback bookkeeping, WSI layout fixes. |
| `vn_queue.c`, `vn_queue.h` | Queue submit, submit2 conversion, feedback command insertion, fence/semaphore/event behavior, external sync payloads. |
| `vn_device_memory.c`, `vn_device_memory.h` | Memory allocation/import/export/map policy, renderer BO creation, guest-vram path, flush/invalidate, mapping assumptions. |
| `vn_buffer.c`, `vn_buffer.h` | Buffer object model, memory-requirements cache, async create on cache hit, binding policy. |
| `vn_image.c`, `vn_image.h` | Image object model, memory/image-format caches, async create on cache hit, deferred image behavior. |
| `vn_wsi.c`, `vn_wsi.h` | Mesa WSI integration, swapchain wrapper, acquire/present policy, async present thread. |
| `vn_ring.c`, `vn_ring.h` | Single-producer/single-consumer command ring, direct and indirect command-stream submission, reply shmem, roundtrip support. |
| `vn_cs.c`, `vn_cs.h` | Command-stream encoder/decoder abstraction, shmem-backed encoder growth, protocol capability tracking. |
| `vn_renderer*.c`, `vn_renderer*.h` | Renderer transport abstraction plus virtgpu and vtest implementations. |

The Venus driver is organized mostly by Vulkan object family. That works well
for a frontend because each file owns the local object metadata and the command
completion policy for that family.

### Generated Venus Protocol

Directory: `src/virtio/venus-protocol`

The checked-in headers are generated by the external `venus-protocol` project.
They define:

- `vn_sizeof_*` helpers for command and struct payload sizes.
- `vn_encode_*` helpers for writing command payloads into `vn_cs_encoder`.
- `vn_decode_*` helpers for reading replies.
- `vn_submit_vk*` helpers for ring submission setup.
- `vn_call_vk*` helpers for synchronous command/reply paths.
- `vn_async_vk*` helpers for no-reply command paths.
- Handle encoding as 64-bit Venus object IDs, not raw guest pointers.

Example: `vn_protocol_driver_device.h` contains generated command functions for
`vkCreateDevice`: `vn_sizeof_vkCreateDevice`, `vn_encode_vkCreateDevice`,
`vn_decode_vkCreateDevice_reply`, `vn_submit_vkCreateDevice`,
`vn_call_vkCreateDevice`, and `vn_async_vkCreateDevice`.

Example: `vn_protocol_driver_queue.h` contains the same generated split for
`vkQueueSubmit` and `vkQueueSubmit2`.

This split is directly relevant to `vkfwd`: generated payload code should not
force one completion policy. The same generated command schema can support
async, sync-call, and lower-level submit modes.

### Mesa Vulkan Runtime

Directory: `src/vulkan/runtime`

Mesa's Vulkan runtime is the shared frontend substrate used by several Mesa
Vulkan drivers. The relevant pieces for `vkfwd` are:

- `vk_object.*`: common base object with loader dispatch storage, object type,
  client-visible state, device/instance backpointers, debug/private-data fields,
  and handle cast helpers.
- `vk_instance.*`: instance initialization, extension validation, app-info
  capture, debug callbacks, instance dispatch table setup, common entrypoints.
- `vk_device.*`: device initialization, enabled feature/extension tracking,
  queue submit mode policy, timeline semaphore mode, common device entrypoints.
- `vk_physical_device.*`: physical-device object state and properties.
- `vk_queue.*`: common queue and submit plumbing.
- `vk_command_pool.*` and `vk_command_buffer.*`: common command pool/buffer
  state and lifecycle.
- `vk_sync*`, `vk_fence.*`, `vk_semaphore.*`: synchronization helpers.
- `vk_render_pass.*`, `vk_graphics_state.*`, `vk_descriptors.*`,
  `vk_pipeline_layout.*`: reusable state modeling.

The runtime is not a remote-execution layer by itself. Its value is that it
shows how much Vulkan state can be owned locally in a driver-frontend shape.

### Mesa Vulkan Util/Generation

Directory: `src/vulkan/util`

Important files:

- `vk_entrypoints_gen.py`
- `vk_dispatch_table_gen.py`
- `vk_extensions_gen.py`
- `vk_icd_gen.py`
- `vk_entrypoints.py`

Mesa generates:

- extension tables from `vk.xml`;
- instance, physical-device, and device entrypoint tables;
- dispatch tables that compact aliases into unions;
- static entrypoint lookup helpers for `vkGetInstanceProcAddr` and
  `vkGetDeviceProcAddr`;
- ICD manifests.

Mesa's dispatch docs emphasize that entrypoint tables and dispatch tables are
separate. Entrypoint tables expose every API spelling; dispatch tables compact
aliases. Drivers layer their own entrypoints, WSI entrypoints, and
`vk_common_*` fallback implementations into a final dispatch table.

## Frontend Object Model

Venus local handles are real local driver objects. The app sees handles that
are pointers to Venus/Mesa structs, not host Vulkan handles. Those structs embed
Mesa runtime base objects and carry a Venus object ID:

- `struct vn_instance_base { struct vk_instance vk; vn_object_id id; }`
- `struct vn_physical_device_base { struct vk_physical_device vk; vn_object_id id; }`
- `struct vn_device_base { struct vk_device vk; vn_object_id id; }`
- `struct vn_queue_base { struct vk_queue vk; vn_object_id id; }`
- `struct vn_command_buffer_base { struct vk_command_buffer vk; vn_object_id id; }`
- `struct vn_device_memory_base { struct vk_device_memory vk; vn_object_id id; }`
- `struct vn_image_base { struct vk_image vk; vn_object_id id; }`
- `struct vn_object_base { struct vk_object_base vk; vn_object_id id; }`

`vn_get_next_obj_id()` assigns monotonically increasing 64-bit IDs. Generated
protocol handle helpers call `vn_cs_handle_load_id()` when encoding a handle and
`vn_cs_handle_store_id()` when decoding returned handles.

Implication for `vkfwd`:

- The source-side frontend should allocate local handles immediately.
- The wire format should use source object IDs, never source pointers or
  receiver handles.
- Dispatchable handles should preserve loader/dispatch-table invariants while
  carrying `vkfwd` identity and frontend metadata.
- The receiver must independently map source object IDs to receiver Vulkan
  handles.

## Dispatch and Loader Integration

Venus is an ICD, not a layer. `vn_icd.c` exposes `vk_icdGetInstanceProcAddr`,
which delegates to `vn_GetInstanceProcAddr`. Instance and device creation build
dispatch tables from generated Venus entrypoints and Mesa WSI entrypoints:

- `vn_CreateInstance` builds a `vk_instance_dispatch_table` from
  `vn_instance_entrypoints`, then adds `wsi_instance_entrypoints`.
- `vn_CreateDevice` builds a `vk_device_dispatch_table` from
  `vn_device_entrypoints`, then adds `wsi_device_entrypoints`.
- Mesa runtime initialization adds `vk_common_*` entrypoints without
  overwriting driver-provided implementations.

This gives Venus a complete frontend dispatch surface. It is not intercepting
another ICD; it is the implementation from the app's perspective.

Implication for `vkfwd`:

- An explicit layer can reuse the table-generation pattern, but it has a
  different loader-chain contract than an ICD.
- A performance frontend may eventually look more ICD-like internally even if
  deployed as a layer initially.
- The generated dispatch layer should keep entrypoint lookup, alias compaction,
  extension gating, and device/instance dispatch levels explicit.

## Command Stream Model

### Encoder

`vn_cs_encoder` is the local command-stream writer. It can target:

- external pointer storage for small stack/local commands;
- dynamically allocated shared-memory arrays;
- suballocated shared-memory pools.

It stores a list of buffers, current write pointer, committed size, sticky
fatal-error state, and storage type. It grows by allocating or suballocating
renderer shmem. `vn_cs_encoder_commit()` seals currently written data so it can
be submitted.

Generated protocol functions reserve and write compact binary command payloads
into this encoder. The generated code owns the Vulkan pointer/array/`pNext`
serialization policy for the supported protocol surface.

### Ring

`vn_ring` is the transport-level submission path. The comments describe it as a
single-producer/single-consumer circular buffer. The frontend writes Venus
commands into a shared-memory ring; an external mechanism notifies the renderer.

Important features:

- Small command streams can be written directly into the ring buffer.
- Large command streams are submitted indirectly through
  `vkExecuteCommandStreamsMESA`, which references shmem resource IDs and
  offsets.
- Ring submissions retain shmem references until the renderer's head passes the
  relevant sequence number.
- Reply paths allocate reply shmem, set it with
  `vkSetReplyCommandStreamMESA`, submit the command, and wait for the ring
  sequence number.
- Roundtrips use `vkSubmitVirtqueueSeqnoMESA` and
  `vkWaitVirtqueueSeqnoMESA`.

Implication for `vkfwd`:

- A compact command-stream abstraction can be independent from the final
  transport.
- Direct-vs-indirect submission is a useful split. Small commands should not
  allocate; large command-buffer payloads should live in stable owned buffers.
- Reply storage needs explicit lifetime and ownership, separate from command
  payload storage.
- Keep a cheap roundtrip primitive for memory/resource-ordering edges, but do
  not use it as the default call completion mode.

## Command Buffer Frontend

Venus command buffers are the clearest match for `vkfwd`'s desired model.

`struct vn_command_buffer` contains:

- Mesa `vk_command_buffer` base state;
- a `vn_cs_encoder cs`;
- a `vn_command_buffer_builder` that tracks render-pass/dynamic-rendering
  state, WSI images needing layout fixes, simultaneous-use flag, and query
  records;
- an optional linked query-feedback command.

Most `vkCmd*` entrypoints use the `VN_CMD_ENQUEUE` macro:

1. Cast the Vulkan command buffer handle to `struct vn_command_buffer`.
2. Compute payload size with `vn_sizeof_vkCmd*`.
3. Reserve encoder space.
4. Encode the command with `vn_encode_vkCmd*`.
5. Only submit immediately if `VN_PERF(NO_CMD_BATCHING)` disables batching.

`vn_BeginCommandBuffer` resets local state, sanitizes begin info, encodes
`vkBeginCommandBuffer`, and marks the command buffer recording.

`vn_EndCommandBuffer` encodes `vkEndCommandBuffer`, commits the encoder, sends
the accumulated command stream through the primary ring, resets the encoder,
and marks the command buffer executable.

This is exactly the frontend pattern `vkfwd` wants: high-frequency `vkCmd*`
recording calls do not roundtrip. They only append to local owned command
storage. Receiver-side materialization is decoupled from each API call.

Important local frontend state:

- command-buffer lifecycle;
- render pass / dynamic rendering state for feedback and WSI fixups;
- query record tracking for later feedback command insertion;
- WSI present layout scrubbing;
- simultaneous-use flag because it affects feedback command lifetime.

Implication for `vkfwd`:

- Start the performance path with command-buffer recording, not with broad
  object creation.
- Generated `vkCmd*` packers can append directly into a command-buffer-local
  arena/stream.
- End/submit boundaries should own command-buffer payload lifetime.
- Some “recording” commands still need local semantic tracking beyond raw
  encoding, especially query, render pass, secondary-command-buffer, and WSI
  layout interactions.

## Queue Submit and Feedback

Venus queue submission is not just “send vkQueueSubmit”. `vn_queue.c` wraps
submission with a frontend policy layer.

The `vn_queue_submission` object describes the submitted batches plus temporary
storage needed to modify them. Venus may:

- inspect wait/signal semaphores;
- import external sync before submission;
- append query feedback commands;
- append timeline semaphore feedback commands;
- append fence feedback commands;
- patch device-group `pNext` data when it adds feedback command buffers;
- cache temporary storage on the queue object to avoid repeated allocation;
- submit asynchronously unless `VN_PERF(NO_ASYNC_QUEUE_SUBMIT)` is set.

Fence and semaphore polling is optimized through feedback slots:

- A fence can have a feedback slot updated by a small GPU command appended to
  the submission.
- `vkGetFenceStatus` can read the local feedback slot first.
- If the feedback slot indicates success, Venus sends an async wait to the
  renderer to close validation/synchronization races.
- If feedback stalls beyond a relax threshold, Venus falls back to synchronous
  renderer calls to detect device loss.
- Timeline semaphores use a similar feedback counter and cached
  `signaled_counter` to avoid spamming redundant waits.

Implication for `vkfwd`:

- Sync status calls do not have to be naive roundtrips every time.
- A local feedback/mirror mechanism can turn common polling into cheap local
  reads, with occasional validation roundtrips.
- Feedback state is subtle: it needs ordering rules, fallback paths, device-lost
  detection, and monotonic counter handling.
- Queue submit is the natural place to splice extra receiver-side commands that
  maintain frontend-visible status.

## Object Creation and Caching

Venus has a nuanced split between synchronous and asynchronous object creation.

Examples:

- `vkCreateCommandPool` allocates a local command pool, initializes local
  bookkeeping, sends `vn_async_vkCreateCommandPool`, and returns success.
- `vkAllocateCommandBuffers` allocates local command-buffer wrappers, assigns
  handles, sends `vn_async_vkAllocateCommandBuffers`, and returns success.
- `vkCreateBuffer` is synchronous on cache miss because Venus needs memory
  requirements. On cache hit, it sends `vn_async_vkCreateBuffer` and returns
  success using cached requirements.
- `vkCreateImage` similarly uses an image memory-requirements cache; cache hit
  enables async create, cache miss is synchronous.
- `vkCreateImageView`, `vkCreateSampler`, `vkCreateFramebuffer`, and many other
  object creates allocate a local object and submit async creation.

Venus often returns success before the renderer has completed the operation.
That is safe only because either:

- the operation is expected not to fail under the negotiated/cached state;
- a later dependency boundary will observe failure/device loss;
- the object family has enough local data for immediate app-visible queries;
- cache miss paths force synchronous truth where required.

Implication for `vkfwd`:

- `vkfwd` should separate local identity allocation from receiver realization.
- Cache correctness is the key to turning result-producing creation paths into
  async paths.
- Deferred receiver failure policy must be explicit. A local success return is
  a contract.

## Physical Device and Query Caches

Venus initializes physical-device state once and serves many queries locally.

During physical-device initialization it queries the renderer for:

- renderer extension list and spec versions;
- renderer API/device version;
- supported extensions after Venus filtering;
- features;
- properties;
- queue family properties;
- memory properties;
- external memory/fence/semaphore support;
- WSI device state.

Then runtime query paths often return cached data:

- `vkEnumeratePhysicalDevices` and device groups are cached under the instance.
- `vkGetPhysicalDeviceProperties2` uses Mesa's common cached properties, then
  patches layered API properties.
- `vkGetPhysicalDeviceQueueFamilyProperties2` returns cached queue-family
  properties.
- `vkGetPhysicalDeviceMemoryProperties2` returns cached invariant memory
  properties, but queries the renderer when memory-budget data is requested.
- `vkGetPhysicalDeviceFormatProperties2` uses a sparse-array cache keyed by
  format and supported result `pNext` shape.
- `vkGetPhysicalDeviceImageFormatProperties2` uses a BLAKE3 key over relevant
  input parameters and supported `pNext` payloads, caches both success and
  format-not-supported results, and sanitizes external-memory properties.

The cache policy is careful about `pNext`. Unknown result/input structs usually
disable caching. This is a good rule for `vkfwd`.

Implication for `vkfwd`:

- The frontend should have an explicit capability snapshot negotiated at
  instance/device creation.
- Query caches must include `pNext` structure identity and meaningful member
  data, not just the base struct.
- Dynamic query outputs, such as memory budget, should remain synchronous or
  explicitly freshness-bounded.
- Unsupported or unmodeled `pNext` should disable cache use rather than return
  stale or incomplete data.

## Memory Model

Venus memory handling is powerful but tightly coupled to Virtio-GPU.

`struct vn_device_memory` tracks:

- Mesa `vk_device_memory` base;
- optional renderer BO;
- ring seqno for ordering BO creation after renderer `vkAllocateMemory`;
- virtqueue roundtrip seqno for ordering `vkFreeMemory` after export/import;
- map range end;
- optional WSI dedicated image.

Allocation modes:

- Simple allocation: submit `vkAllocateMemory`, often async, record ring seqno.
- Export allocation: allocate memory, create renderer BO, submit roundtrip so
  export/free ordering is safe.
- Guest VRAM path: create renderer BO first, import it into renderer Vulkan
  memory through Venus protocol, and roundtrip before allocation.
- dma-buf import: create renderer BO from fd, roundtrip, then import resource
  into renderer Vulkan memory.

Mapping behavior:

- BO creation is deferred until `vkMapMemory` when possible to avoid cost for
  HOST_VISIBLE allocations that are never mapped.
- Mapping a BO may block until the renderer creates and injects pages into the
  guest.
- The code explicitly comments that assuming a renderer BO can be created for
  any mappable renderer `VkDeviceMemory` is wrong and needs a future extension.
- `vkUnmapMemory2` is effectively a no-op.
- Flush/invalidate call renderer BO flush/invalidate hooks; virtgpu backend
  treats them as no-ops because its kernel mapping is coherent.

Mesa's public Venus docs warn that Venus relies on implementation-defined
behavior around HOST_VISIBLE memory and exportable/mappable dma-buf/blob
resources.

Implication for `vkfwd`:

- Do not copy Venus memory semantics directly unless `vkfwd` targets the same
  VM/virtio resource model.
- `vkfwd` needs its own mapped-memory policy: mirror, explicit upload stream,
  shared-memory transport, page tracking, or staged copy.
- Treat `vkMapMemory` as a major architecture boundary, not as a simple remote
  pointer return.
- Record ordering edges between memory allocation, external resource creation,
  mapping, export, and free explicitly.

## WSI and Presentation

Venus reuses Mesa common WSI and wraps swapchain state locally.

Key pieces:

- `vn_wsi_init` initializes `wsi_device` and mutates advertised device
  properties/extensions for backend quirks.
- `vn_CreateSwapchainKHR` delegates to common WSI, then creates a local
  `vn_swapchain` wrapper with locks for acquire/present coordination.
- `vn_AcquireNextImage2KHR` delegates to common WSI, then imports sync fds into
  the requested semaphore/fence when implicit fencing is unavailable.
- `vn_QueuePresentKHR` can run asynchronously on a per-queue present thread
  unless disabled or implicit fencing is available.
- `vn_wsi_flush` forces non-present queue users to wait until async present has
  released queue access.

This is sophisticated but very platform-specific. It solves a VM/guest WSI
problem with common Mesa WSI, dma-buf, sync-file, and compositor behavior.

Implication for `vkfwd`:

- Exclude WSI from the first frontend-stream prototype.
- Later WSI work should be its own design track with platform policy.
- Async present is a useful idea, but the locking and result-delivery contract
  must be designed around `vkfwd`'s display model.

## Renderer Abstraction

`vn_renderer` is the transport/backend abstraction. It exposes operations for:

- submit command batches;
- wait on renderer syncs;
- create/destroy shared memory;
- create/import/export/map/flush/invalidate BOs;
- create/import/export/read/write/reset syncs.

There are two concrete backends in Mesa:

- `vn_renderer_virtgpu.c`: virtio-gpu kernel backend using DRM ioctls,
  resource blobs, dma-buf, syncobj/sync-file, and context submission.
- `vn_renderer_vtest.c`: vtest backend for development without full VM setup.

Renderer info carries protocol and host capabilities:

- wire format version;
- Vulkan XML version;
- `VK_EXT_command_serialization` spec version;
- `VK_MESA_venus_protocol` spec version;
- supported protocol extension mask;
- max timeline count;
- dma-buf import support;
- external sync support;
- implicit fencing support;
- guest VRAM support.

Implication for `vkfwd`:

- Keep the transport/backend abstraction narrow and capability-driven.
- Negotiate protocol and extension support up front.
- Expose explicit transport capabilities to command policy: shared memory,
  external sync, host-visible memory model, timeline count, and reply support.

## Reuse Assessment for vkfwd

### High-Value Ideas To Copy

- Local source handles with separate protocol IDs.
- Generated command encoders that support async, sync-call, and low-level
  submit variants.
- Command-buffer-local encoding with submit at `vkEndCommandBuffer`.
- Direct/indirect command-stream submission split.
- Query/property caches that include `pNext` shape and disable caching on
  unknown structures.
- Queue-submit feedback commands for fence/semaphore/query status.
- Queue/object cached temporary storage to avoid per-submit allocations.
- Explicit capability negotiation before hot-path command encoding.
- Clear separation between frontend policy and renderer transport ops.

### Possible Code Reuse

- Mesa's generated dispatch-table pattern is a strong reference for `vkfwd`
  generator design, but direct code reuse would bring Mesa build integration.
- The Mesa runtime object model is conceptually valuable. Directly embedding it
  in `vkfwd` would be a major dependency decision and probably too invasive for
  the current scaffold.
- Venus-protocol headers are tightly coupled to Venus/MESA protocol extensions
  and virglrenderer. They are best treated as reference material unless
  `vkfwd` intentionally targets Venus protocol compatibility.
- The vtest path is useful as a study harness, not as a direct vkfwd backend.

### Things Not To Copy Blindly

- HOST_VISIBLE memory assumptions.
- dma-buf/blob resource model.
- Virtio-GPU ring and kernel IOCTL dependencies.
- Mesa common WSI assumptions.
- Returning local success before receiver success without a defined deferred
  failure policy.
- Protocol-specific extensions such as `VK_MESA_venus_protocol` as a general
  `vkfwd` wire format.

## Recommended vkfwd Architecture Adjustments

1. Keep current generated API forwarding as a correctness scaffold.
2. Add a frontend completion taxonomy to generated command metadata:
   `local-immediate`, `deferred-stream`, `batched-submit`, `cached-query`,
   `feedback-local`, `required-round-trip`, `unsupported-explicit`.
3. Model source handles as local frontend objects with stable source IDs.
4. Start the performance path with command buffers:
   - local command-buffer object;
   - command-buffer-owned payload arena;
   - generated `vkCmd*` appenders;
   - submit payload at `vkEndCommandBuffer`;
   - queue submission references already-materialized command-buffer payloads.
5. Add a query/property cache framework with conservative `pNext` keys.
6. Add receiver realization state to each local object:
   - not submitted;
   - submitted;
   - realized;
   - failed/deferred device-lost.
7. Defer WSI and mapped-memory optimization until command buffers and object
   identity are working.
8. Treat memory mapping as its own design, not a Venus clone.
9. Use feedback slots as a second-stage optimization for fences, semaphores,
   events, and queries once basic queue ordering works.

## Immediate Prototype Suggested From Mesa Findings

The first Venus-inspired `vkfwd` prototype should be intentionally smaller than
Venus:

- Synchronous setup:
  - instance creation;
  - physical-device enumeration;
  - feature/property snapshot;
  - device creation.
- Local object IDs:
  - command pool;
  - command buffer;
  - fence;
  - buffer/image handles as needed by a tiny workload.
- Deferred command-buffer path:
  - `vkBeginCommandBuffer`;
  - a small set of `vkCmd*` draw/copy/barrier commands;
  - `vkEndCommandBuffer` materializes one owned stream payload.
- Queue submit:
  - submit encoded command-buffer payload;
  - use synchronous fence completion first;
  - replace with feedback slots later.
- One cache:
  - `vkGetPhysicalDeviceFormatProperties2` or buffer memory requirements with
    a strict key and cache disabled on unknown `pNext`.

Do not include swapchains or mapped-memory readback in the first prototype.
Venus shows both are large enough to deserve separate architecture slices.

## Bottom Line

Mesa/Venus validates the wild idea: Vulkan remote execution performs best when
the source side is a real frontend, not a per-call RPC shim. Venus completes
many calls locally, streams command data asynchronously, and reserves roundtrips
for setup truth, cache misses, replies, synchronization, memory/resource
ordering, and WSI edges.

For `vkfwd`, the strongest path is a native frontend stream inspired by Venus,
not direct Venus adoption. Copy the shape: local objects, generated encoders,
command-buffer batching, conservative caches, queue feedback, negotiated
transport capabilities. Avoid copying the Virtio-GPU memory and WSI assumptions
unless `vkfwd` deliberately becomes a Venus-compatible guest driver.
