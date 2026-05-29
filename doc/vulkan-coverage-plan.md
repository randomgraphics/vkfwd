# Vulkan Coverage Plan

This document is the working plan for growing `vkfwd` from a small Vulkan
layer scaffold into broad Vulkan API capture, serialization, forwarding, and
receiver-side replay. It is written for both humans and agents: use it to pick
the next unit of work, decide what belongs in generated code, and avoid
mistaking trace-only placeholders for complete forwarding.

The core rule is that Vulkan coverage must be generator-led from one pinned
Vulkan API version. Hand-written interceptors are acceptable for early
scaffolding and semantic special cases, but the full Vulkan command and
structure surface is too large and too version-sensitive to maintain by hand.

The selected Vulkan API version is a repository-wide contract. Generated code,
coverage policy, serializer command ids, `pNext` structure handling, replay
records, tests, documentation, and build assumptions must all agree with that
one version. Updating the Vulkan header or `vk.xml` is therefore not a local
dependency refresh; it is an API-version migration that must regenerate and
revalidate the repository against the new specification.

## Goals

- Intercept Vulkan commands through a conforming explicit layer.
- Build all generated Vulkan knowledge from one explicitly selected Vulkan API
  version.
- Preserve Vulkan loader-chain and dispatch-table invariants.
- Serialize every parameter needed to replay a command after the original
  application-owned memory is gone.
- Deserialize serialized calls into receiver-owned replay records.
- Replay against a receiver-side Vulkan implementation using source-to-receiver
  handle mappings.
- Track unsupported commands and extensions explicitly.
- Keep trace-only behavior visibly separate from real forwarding and replay.

## Non-Goals For Early Milestones

- Perfect remote rendering for arbitrary Vulkan applications.
- Complete extension support in the first generated pass.
- Network transport design before the capture and replay contracts are stable.
- Optimization of stream size before correctness and ownership are proven.
- Reusing source-side handle values on the receiver.

## Definitions

- **Capture** means intercepting a Vulkan call and copying all data required to
  describe that call independent of the application's stack or heap lifetime.
- **Serialization** means converting captured data into a stable protocol
  payload. A log line is not sufficient for replay.
- **Forwarding** means delivering serialized calls to a sink. The sink may be
  in-process, IPC, network, or file-backed.
- **Replay** means invoking Vulkan calls on a receiver-side Vulkan
  implementation using receiver-owned state and mapped handles.
- **Trace-only** means recording command names or partial metadata for
  inspection. Trace-only paths must not be described as forwarding complete
  Vulkan work.

## Agent Working Rules

When implementing from this plan:

1. Read `AGENTS.md` and preserve the repository comment rule.
2. Prefer generated coverage over adding a broad hand-written command list.
3. Do not update Vulkan headers, `vk.xml`, generated metadata, or generated
   code independently; a Vulkan API-version change must be handled as one
   repository migration.
4. Keep each change tied to a milestone and update this document when coverage
   categories or assumptions change.
5. Add comments that explain Vulkan invariants, pointer lifetime assumptions,
   handle ownership, replay ordering, and placeholder boundaries.
6. Do not silently pass through unsupported commands if the user could confuse
   that behavior with captured replay support.
7. Add or update tests for the coverage class being changed.
8. Keep generated files reproducible from checked-in generator inputs.

## Coverage States

Every Vulkan command should eventually have one explicit state:

| State | Meaning |
| --- | --- |
| `unclassified` | The command exists in the selected Vulkan XML but no policy has been assigned. |
| `passthrough-only` | The layer forwards locally but does not serialize enough information for replay. |
| `trace-only` | The layer records limited metadata for diagnostics only. |
| `capture-ready` | Capture records own all call inputs needed for serialization. |
| `roundtrip-ready` | Capture, serialization, and deserialization preserve the call shape. |
| `replay-ready` | Receiver-side replay invokes the real Vulkan command with mapped handles. |
| `unsupported-explicit` | The command is recognized and intentionally rejected or logged as unsupported. |

The build or test suite should eventually fail if commands remain
`unclassified` for the selected Vulkan XML version.

## Vulkan XML Generation

### Pinned Vulkan Version

The repository must record exactly which Vulkan API version drives generation.
That pinned version is the source of truth for:

- Vendored Vulkan headers used at build time.
- `vk.xml` used by the generator.
- Generated command ids and metadata.
- Core and extension command classification.
- Struct, enum, bitmask, and handle definitions.
- `pNext` structure recognition.
- Serialization and replay records.
- Coverage reports and tests.

A Vulkan API-version migration must update these pieces together:

- The vendored Vulkan headers.
- The pinned `vk.xml`.
- Generator expectations and golden metadata tests.
- All generated files.
- Coverage policy for added, removed, promoted, or changed commands and
  structures.
- Wire-format compatibility notes when command ids or payload shapes change.
- Documentation that names the supported Vulkan API version.

Acceptance criteria for a version migration:

- Generated output is reproducible from the new pinned version.
- Coverage reports account for every command in the new `vk.xml`.
- Tests prove the runtime and generated metadata agree on the same version.
- Any unsupported new API surface is explicitly classified.

### Vendored Vulkan Spec Bundle

The repository should contain a single vendored Vulkan specification bundle
that includes both `vk.xml` and the matching standard Vulkan headers from the
same Vulkan-Headers revision. Project source should include these vendored
headers directly through the build include path instead of using system Vulkan
headers.

Suggested layout:

```text
src/third_party/vulkan/
  VERSION
  registry/vk.xml
  include/vulkan/
  include/vk_video/
  LICENSE.md
```

Rules:

- `src/third_party/vulkan/VERSION` records the Vulkan API version, upstream source,
  upstream revision, and import date.
- `src/third_party/vulkan/registry/vk.xml` is the only XML registry input used by
  the generator.
- `src/third_party/vulkan/include` is the first Vulkan include directory for all
  `vkfwd` targets. The whole include tree is vendored because standard Vulkan
  headers may reference sibling directories such as `vk_video`.
- Source files may continue to write standard includes such as
  `#include <vulkan/vulkan.h>`, but CMake must resolve them to the vendored
  headers.
- The build must not accidentally pick up `/usr/include/vulkan` or SDK headers
  before the vendored include directory.
- The project may still link to the system Vulkan loader library. The loader is
  a runtime ABI boundary; the compile-time API shape comes from the vendored
  headers and matching `vk.xml`.
- Updating anything under this bundle is an API-version migration and must
  regenerate metadata, rerun coverage checks, and update documentation.

### Inputs

- A pinned Vulkan API version.
- Vendored `vk.xml` matching that version.
- Vendored Vulkan headers matching that version.
- Optional allowlists or denylists for platform extensions.
- A local policy file that assigns coverage states and semantic categories.

### Generated Outputs

- Command identifiers.
- Command metadata tables.
- Dispatch table structures.
- Global, instance, and device entry point declarations.
- `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` lookup tables.
- Capture record types.
- Serializer and deserializer skeletons.
- Replay executor skeletons.
- Coverage reports for commands, structs, handles, enums, and extensions.

Generated command code should be split by API command where practical. A
command such as `vkCreateDevice` should have its own generated declaration and
implementation files so review, testing, and future replay policy can stay
local to that command.

Generated code belongs under the generator-owned output tree:

```text
src/vkfwd/generated/
```

Per-command generated code belongs under:

```text
src/vkfwd/generated/commands/
```

Per-command generated metadata should live beside the command's generated
source files. Do not add one centralized all-command metadata JSON; it will
become too large and hard to review as coverage grows toward the full Vulkan
API surface. Small global manifests are acceptable for generator provenance,
protocol version, Vulkan version, and the list of generated command artifacts.

Every file in `src/vkfwd/generated/` is generator-owned. Do not place
hand-written code there, and do not expect local edits to survive regeneration.
The generated root must contain a README that repeats this rule because
generated command files will become numerous and easy to mistake for normal
source.

### Generator Requirements

- Generated output must be deterministic.
- Generated files should include the Vulkan XML version and generator version.
- Generated files must also include the pinned Vulkan API version they were
  produced from.
- Manual edits to generated files should be avoided.
- Generator tests should cover optional pointers, counted arrays, strings,
  handles, output parameters, `pNext`, and platform-guarded commands.
- Regeneration tests must compare all checked-in generated artifacts against a
  temporary fresh generation.
- Generated code may include stable extension points for human code, but the
  generator must never create, overwrite, delete, or disable human-owned hook
  implementations.

### Command Metadata Model

The normalized generator metadata should include enough information for
dispatch, capture, serialization, deserialization, replay, coverage, and
compatibility decisions without repeatedly re-parsing XML in runtime code.

Required command metadata:

- Stable command id.
- Command name.
- Return type.
- Command level: global, instance, or device.
- Dispatch parameter, if any.
- Source API dialect, such as standard Vulkan versus Vulkan SC.
- Success and error codes.
- Created or destroyed handle types.
- Parameter list with declaration, base type, pointer depth, constness,
  optionality, length expression, direction, handle kind, and handle parent.
- Coverage state.

Required type metadata:

- Dispatchable and non-dispatchable handle classification.
- Handle parent relationships and object type enum.
- Struct members, including optional fields, count expressions, `sType` values,
  string arrays, pointer depth, and `pNext` participation.

The current `vk.xml` snapshot contains 857 unique command names in the
`<commands>` section. Of those, 834 are required by standard Vulkan
feature/extension declarations. The larger number is the broad command surface
known to this XML; the smaller number is not additive because it is a subset.

Useful planning groups for the 857-command surface are:

| Group | Count |
| --- | ---: |
| Loader and API entry lookup | 3 |
| Debug, validation, tooling, labels, private data | 44 |
| Surface, display, swapchain, presentation | 83 |
| Command-buffer drawing, dispatch, transfer, clear/resolve | 90 |
| Command-buffer dynamic state and binding | 140 |
| Command-buffer render pass/rendering scope | 19 |
| Command-buffer sync/query/timestamp commands | 17 |
| Acceleration structure, ray tracing, micromap | 43 |
| Video encode/decode | 16 |
| Queue submission, queue waits, sparse binding | 9 |
| Command pools and command buffer lifecycle | 11 |
| Instance, physical-device, device lifecycle/capability queries | 51 |
| Pipelines, shaders, pipeline cache/binaries | 50 |
| Descriptors and pipeline layout | 29 |
| Buffers, images, views, samplers, tensors | 60 |
| Memory allocation, binding, mapping, requirements, addresses | 63 |
| Synchronization objects and host waits/signals | 43 |
| Queries, performance, timestamps, calibration, latency | 26 |
| External platform handles and interop | 17 |
| Other extension/device utility commands | 43 |

This grouping is a planning taxonomy, not a Khronos-owned classification.
Eventually it should be generated and then corrected by a local policy file for
commands whose names do not fully describe replay behavior.

### Manual Hook Policy

Generated per-command pack, transfer, unpack, dispatch, and replay code must
provide optional human intervention points. The default behavior must compile
to no hook calls, no branches, and no runtime work when no manual hook is
enabled.

The current hook mechanism uses generated compile-time traits and `if
constexpr` guards. The default trait values disable each hook point. Human code
enables a hook by specializing the command hook trait and setting the relevant
`*_enabled` constant to `true`.

Hook points should exist at least around these phases:

- Before parameters are packed.
- After parameters are packed.
- Before a packet is unpacked.
- After a packet is unpacked.

Future generated dispatch and replay stages should follow the same pattern:
manual intervention is possible where Vulkan semantics require it, but absent
manual code must not add runtime overhead.

Human-owned hook files belong under a dedicated directory, with one clearly
named header per API command:

```text
src/vkfwd/hooks/<api>Hooks.hpp
```

For example:

```text
src/vkfwd/hooks/vkCreateDeviceHooks.hpp
```

If hook code needs out-of-line bodies, add a matching `.cpp` file manually and
wire it into the build. The generator may conditionally include hook headers if
they exist, but it must not generate or overwrite them. If human hook code
breaks compilation, the build should fail plainly so the human owner can fix
the hook.

## Dispatch And Loader Plan

Vulkan dispatch correctness is a foundation milestone because all later capture
depends on command lookup reaching the right next function.

### Global Commands

- Export `vkGetInstanceProcAddr`.
- Export `vkGetDeviceProcAddr`.
- Export layer negotiation entry points when required by the loader.
- Intercept global commands that exist before an instance is created.

### Instance Commands

- During `vkCreateInstance`, consume the loader link info exactly once.
- Store the next `vkGetInstanceProcAddr` per created `VkInstance`.
- Install generated instance dispatch entries after successful creation.
- Remove instance dispatch state after `vkDestroyInstance` is forwarded.
- Document why the loader-owned `pNext` link is advanced and why application
  `pNext` payloads must remain intact.

### Device Commands

- During `vkCreateDevice`, obtain the next `vkGetDeviceProcAddr`.
- Store generated device dispatch entries per created `VkDevice`.
- Map child dispatchable handles such as queues and command buffers back to
  their owning device dispatch table.
- Remove device dispatch state after `vkDestroyDevice` is forwarded.
- Treat device dispatch as mandatory for meaningful Vulkan workload coverage.

## Serialization Protocol Plan

The protocol should be versioned before any serious replay work begins.
The detailed pack/unpack design, hot-path expectations, stream compatibility
rules, ownership requirements, and manual hook contract live in
`doc/api-pack-unpack-design.md`.

Required fields:

- Command id.
- Monotonic call sequence number.
- Direction: host-to-receiver or receiver-to-host.
- Thread id or ordering domain when needed.
- Encoded parameter payload.
- Result/output payload when the capture policy requires post-call data.

Before command streaming begins, the dispatcher and receiver must complete a
handshake. The receiver/replay runtime is expected to outlive individual
interceptor builds, so it must be able to accept streams produced by all
compatible interceptor/dispatcher builds.

The handshake should contain:

- Magic value.
- Wire major and minor version.
- Vulkan API major, minor, and patch version used by the generator.
- Generator schema version.

Once the handshake succeeds, the rest of the command stream is assumed to
follow the negotiated version. Per-command packets must not repeat stream
version information or redo stream compatibility validation in the hot path.
Command-specific decoding may still reject unknown command ids, unsupported
payload revisions, missing extensions, or unimplemented replay policies.

Compatibility rules:

- Wire major changes are breaking unless the receiver explicitly supports that
  older major.
- Wire minor changes are readable only inside the receiver's declared readable
  minor range.
- Vulkan major version must match.
- Vulkan minor versions are backward-compatible only when the receiver was
  generated against the same or newer minor version. A receiver generated
  against Vulkan 1.4 may read Vulkan 1.3 streams if the wire format is
  compatible, but a Vulkan 1.3 receiver must reject a Vulkan 1.4 stream unless
  a deliberate compatibility shim exists.
- Vulkan patch/header differences are recorded for diagnostics and exact
  replay policy, but they should not by themselves imply incompatibility inside
  the same supported major/minor line.
The current scaffold records this boundary in `src/vkfwd/protocol.hpp` and
generated code exposes a `current_handshake()` helper. That is only the
foundation; the real receiver parser still needs handshake exchange, a
multi-version command table, and payload adapters.

Parameter encoding must support:

- Fixed-width scalars.
- Vulkan enums and bitmasks.
- Dispatchable and non-dispatchable handles.
- Optional pointers.
- Counted arrays.
- Null-terminated strings and arrays of strings.
- Nested structs.
- Known `pNext` chains.
- Opaque byte ranges for mapped or host-visible memory payloads.
- Output parameters whose replay value is produced by the receiver.

The protocol should not assume host and receiver have the same pointer size,
endianness, handle representation, memory addresses, or driver object identity.

## Capture Ownership Rules

By the time a captured call reaches a forwarding sink:

- No parameter may depend on application stack memory remaining alive.
- No borrowed pointer may be stored without an ownership comment and a bounded
  synchronous lifetime.
- Counted arrays must copy the count and all referenced elements.
- Strings must be copied, including nullability.
- Known `pNext` chains must be deep-copied by structure type.
- Unknown `pNext` structures must be classified as unsupported, opaque
  passthrough, or explicitly ignored with rationale.
- Host memory payloads must define whether the bytes are captured before the
  call, after the call, or around synchronization events.

## Replay And Handle Mapping

Receiver replay must maintain handle maps instead of reusing source values.

Handle table categories:

- Instance-level dispatchable handles.
- Device-level dispatchable handles.
- Non-dispatchable handles.
- Externally imported or exported handles.
- Swapchain and presentation handles.
- Synthetic handles used only by the protocol.

Replay rules:

- Creation commands add mappings only after receiver-side success.
- Destruction commands remove mappings only after the receiver call has been
  issued or the failure policy has run.
- Commands that return handles need a response policy if the host continues to
  use source-side results.
- Failed source-side calls may still need trace visibility but usually must not
  create receiver-side state.
- Externally synchronized Vulkan objects require explicit ordering assumptions
  in capture and replay code comments.

## Milestones

### Milestone 0: Planning And Baseline

Acceptance criteria:

- This plan exists and is kept current.
- The current scaffold continues to build and pass smoke tests.
- Placeholder serializer, deserializer, and replay executor comments clearly
  say they are not complete forwarding or Vulkan replay.

### Milestone 1: Generator Skeleton

Deliverables:

- Add a generator entry point.
- Import the vendored Vulkan spec bundle with matching headers and `vk.xml`.
- Configure build targets so standard Vulkan includes resolve to the vendored
  headers.
- Parse commands, structs, handles, enums, and extensions.
- Emit a coverage report without changing runtime behavior.
- Add tests for generator determinism and basic metadata extraction.
- Generate a small compiled proof slice before broad API expansion.

Acceptance criteria:

- A developer can regenerate metadata from a clean checkout.
- The coverage report lists every command from the selected Vulkan XML.
- The generated metadata, build headers, and coverage report name the same
  Vulkan API version.
- A build check verifies the compiler is using the vendored Vulkan headers.
- Commands are classified as `unclassified` until policy is added.
- The proof slice emits deterministic metadata and compiled C++ for
  `vkCreateInstance` and `vkCreateDevice`.
- Generated command files live only under `src/vkfwd/generated/commands/`.
- Human-owned hook files live outside generated output and survive
  regeneration.
- Generated code exposes the current handshake metadata, while per-command
  pack/unpack records avoid repeated stream compatibility data.
- Empty hook defaults compile out without runtime calls or branches.

Current proof-slice status:

- `dev/generator/vulkan_metadata.py` parses the pinned `vk.xml`.
- The generator selects the standard Vulkan variant when Vulkan SC defines a
  same-named command with a different contract.
- Generated metadata, coverage, command info, dispatch helper, and per-command
  pack/unpack files exist for `vkCreateInstance` and `vkCreateDevice`.
- Generated per-command code is compiled into `vkfwd_capture`.
- A human-owned no-op hook specialization exists for `vkCreateDevice` at
  `src/vkfwd/hooks/vkCreateDeviceHooks.hpp`.
- The test suite regenerates into a temporary directory and compares generated
  artifacts byte-for-byte.
- The current pack/unpack slice is intentionally shallow. Pointer-bearing
  parameters, counted arrays, strings, and `pNext` chains still need generated
  deep-copy serialization before the stream can be considered replay-stable.

### Milestone 2: Generated Dispatch Tables

Deliverables:

- Generate global, instance, and device dispatch declarations.
- Generate command lookup tables for `vkGetInstanceProcAddr` and
  `vkGetDeviceProcAddr`.
- Replace narrow hand-written lookup branches with generated lookup while
  preserving current loader-chain behavior.

Acceptance criteria:

- Existing instance smoke behavior still works.
- Device command lookup is no longer a stub.
- Unsupported commands intentionally fall through to the next layer or are
  reported according to policy.

### Milestone 3: Core Lifecycle Capture

Initial command families:

- Instance creation and destruction.
- Physical device enumeration.
- Physical device properties, features, queue families, memory properties.
- Device creation and destruction.
- Queue retrieval.

Acceptance criteria:

- Capture records own all input data for these calls.
- Serialization round-trips command shape and key parameters.
- Source-to-receiver mappings exist for instance, physical device, device, and
  queue handles.

### Milestone 4: Resource And Memory Capture

Initial command families:

- Buffers and buffer views.
- Images and image views.
- Device memory allocation and free.
- Bind buffer and image memory.
- Map, unmap, flush, and invalidate mapped memory ranges.
- Samplers.

Acceptance criteria:

- Memory ownership and byte capture timing are documented per command family.
- Receiver replay can reconstruct basic buffers, images, and memory bindings.
- Tests cover mapped memory payload capture and handle remapping.

### Milestone 5: Command Buffer Recording

Initial command families:

- Command pool creation and reset.
- Command buffer allocation and free.
- Begin and end command buffer.
- Pipeline barriers.
- Copy, blit, clear, fill, and update commands.
- Render pass begin/end or dynamic rendering begin/end, depending on target API.
- Bind pipeline, descriptor sets, vertex buffers, and index buffers.
- Draw, draw indexed, dispatch, and indirect variants.

Acceptance criteria:

- Receiver replay can rebuild command buffers from captured recording calls.
- Command buffer ordering is preserved.
- Externally synchronized command pool and command buffer assumptions are
  documented.

### Milestone 6: Descriptor And Pipeline State

Initial command families:

- Descriptor set layouts, pools, allocation, update, and free.
- Pipeline layouts.
- Shader modules.
- Render passes or dynamic rendering structures.
- Graphics, compute, and pipeline cache creation.
- Framebuffers.

Acceptance criteria:

- Complex nested create-info structures are deep-copied and round-tripped.
- `pNext` chains for common modern features are supported or explicitly
  rejected.
- Pipeline and descriptor handles replay through receiver mappings.

### Milestone 7: Queue Submission And Synchronization

Initial command families:

- Queue submit and submit2.
- Queue wait idle and device wait idle.
- Fences.
- Binary semaphores.
- Timeline semaphores.
- Events.

Acceptance criteria:

- Submission ordering is preserved.
- Synchronization objects are mapped and replayed.
- Host waits and receiver waits have an explicit policy.
- Timeline values and semaphore signal/wait payloads round-trip.

### Milestone 8: Presentation And WSI

Initial command families:

- Surface creation for selected platforms.
- Surface capability and format queries.
- Swapchain creation and destruction.
- Swapchain image retrieval.
- Image acquire.
- Queue present.

Acceptance criteria:

- Platform-specific commands are generated behind correct guards.
- Swapchain image mapping policy is documented.
- Unsupported WSI platforms are explicit rather than silently ignored.

### Milestone 9: Extension Expansion

Deliverables:

- Rank extensions by practical application coverage.
- Add policy for each extension command and structure.
- Support high-value extensions incrementally.

Acceptance criteria:

- Coverage report distinguishes core Vulkan, promoted extensions, and optional
  platform/vendor extensions.
- Unknown extension `pNext` payloads have explicit behavior.

## Testing Plan

Test layers:

- **Structure smoke tests:** repository layout, generated file presence, and
  basic build integration.
- **Generator tests:** metadata parsing, deterministic output, and policy
  classification.
- **Serialization golden tests:** representative binary payloads for scalars,
  arrays, strings, handles, structs, and `pNext`.
- **Round-trip tests:** serialize then deserialize into equivalent replay
  records.
- **Layer loader tests:** confirm the Vulkan loader can discover and call the
  layer.
- **Dispatch tests:** confirm global, instance, and device command lookup uses
  the correct next function.
- **Replay tests:** validate handle mapping and command ordering against a real
  or mocked Vulkan dispatch backend.
- **Integration tests:** run small Vulkan programs that exercise one milestone
  at a time.

## Documentation Requirements

Each milestone should update documentation for:

- Newly covered command families.
- Unsupported or trace-only command families.
- Wire format changes.
- Replay assumptions.
- Known gaps.
- How to regenerate generated files.

Code comments should be concise, but they must preserve design intent where a
future maintainer might otherwise break Vulkan loader, lifetime, ownership,
handle mapping, or replay ordering assumptions.

## Suggested First Implementation Sequence

1. Add a `tools/` or `dev/` generator entry point.
2. Pin the target Vulkan API version.
3. Import matching Vulkan headers and matching `vk.xml` into
   `src/third_party/vulkan/`.
4. Configure CMake so `#include <vulkan/vulkan.h>` resolves to the vendored
   headers for all project targets.
5. Generate a read-only coverage report.
6. Add a policy file that classifies the current scaffold commands.
7. Generate dispatch tables while keeping existing behavior intact.
8. Add device dispatch support.
9. Replace text call names with command ids and minimal structured payloads.
10. Expand lifecycle capture and receiver handle maps.

This sequence keeps the project honest: the first generated artifact is a map
of the whole problem, not a claim that the whole problem is already solved.
