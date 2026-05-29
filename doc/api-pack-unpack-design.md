# API Pack/Unpack Design

This document describes how `vkfwd` should pack intercepted Vulkan API calls
into replayable command payloads and unpack them on the receiver side. This is
one of the core performance-sensitive parts of the library: every captured
Vulkan call crosses this path, and many command-buffer workloads contain very
large numbers of small commands.

The design goal is direct generated code with explicit ownership and versioning
rules. Generic reflection, dynamic maps, string-based dispatch, and per-field
virtual calls should stay out of the hot path.

## Goals

- Generate command-specific pack and unpack code from the pinned Vulkan XML.
- Preserve enough parameter data to replay after application-owned memory is
  gone.
- Keep absent manual hooks at true zero runtime overhead.
- Support human-owned command-specific intervention without regeneration
  overwriting manual code.
- Produce a stream format that one receiver/replay runtime can read across
  compatible interceptor builds.
- Keep the fast path predictable: contiguous writes, bounded branching, and no
  heap churn for small fixed-shape commands.

## Non-Goals

- A C++ reflection layer for Vulkan.
- A self-describing text protocol.
- Reusing source process pointer values or driver handle values on the
  receiver.
- Optimizing stream size before ownership, ordering, and compatibility are
  correct.

## Terminology

- **Parameters** are the immediate C/C++ values passed to a Vulkan command.
- **Packed command** is the generated, command-specific representation produced
  by capture-side pack code.
- **Wire payload** is the byte stream handed to an API endpoint. The endpoint
  may send that payload over IPC, write it to a file, or replay it in-process,
  but those choices sit below the top-level API completion contract.
- **Unpacked command** is the receiver-owned representation produced from the
  wire payload.
- **Replay record** is the data the receiver uses to invoke Vulkan after handle
  mapping and any compatibility adaptation.
- **API endpoint** is the driver-like completion boundary for an intercepted
  call. It may use local replay, remote replay, logs, files, or transport
  internally, but it must complete the caller-visible return value, output
  parameters, handle identities, ordering, and error policy before the
  intercepted command returns when Vulkan semantics require a synchronous
  result.

The current proof slice has generated `Parameters` and `PackedCommand` structs
for `vkCreateInstance` and `vkCreateDevice`, but it is intentionally shallow:
pointer-bearing parameters are copied as pointer values only. That is useful to
prove code generation and hook mechanics, but it is not replay-stable yet.

## Generated Layout

Generated pack/unpack command code lives under:

```text
src/vkfwd/ferry/core/generated/
```

Forwarder-specific generated Vulkan layer entry points, dispatch lookup tables,
and interceptor glue belong under `src/vkfwd/ferry/forwarder/generated/` instead of
the core generated command tree. Keeping generated output near the boundary it serves
prevents loader/dispatch mechanics from being confused with replayable payload
schema.

Per-command generated pack/unpack files live under:

```text
src/vkfwd/ferry/core/generated/command/
```

Per-command generated schema and metadata should live in the same directory as
the command's generated source. Avoid a centralized all-command metadata file;
it will become too large to review once the generator covers hundreds of Vulkan
APIs.

Human-written hooks live outside generated output:

```text
src/vkfwd/ferry/core/hook/<api>Hook.hpp
src/vkfwd/ferry/core/hook/<api>Hook.cpp
```

The generator may conditionally include hook headers with `__has_include`, but
it must never create, overwrite, delete, or disable hook files.

## Hot Path Shape

Generated pack code should be shaped like a direct hand-written function:

1. Load command-specific hook trait.
2. Compile-time call `before_pack` only when enabled.
3. Copy or deep-copy parameters into an owned command-specific packed record.
4. Compile-time call `after_pack` only when enabled.
5. Emit or hand the packed record to serialization.

Generated unpack code mirrors this:

1. Trust the already-negotiated stream/session version.
2. Validate only command-specific payload shape and payload revision.
3. Compile-time call `before_unpack` only when enabled.
4. Read the payload into receiver-owned command-specific data.
5. Compile-time call `after_unpack` only when enabled.
6. Hand the record to replay or a replay adapter.

Absent hooks must compile away. The generated code uses `if constexpr` around
hook calls. Hook specializations must still provide member names for disabled
phases because the current generated pack/unpack functions are not templates,
but disabled phases are not executed.

## Hook Contract

Each command has a specialization of:

```cpp
vkfwd::manual::CommandHooks<vkfwd::generated::CommandId::...>
```

The specialization exposes compile-time booleans:

```cpp
before_pack_enabled
after_pack_enabled
before_unpack_enabled
after_unpack_enabled
```

Only enabled hooks should contain real work. Out-of-line hook bodies are
allowed and preferred for non-trivial logic. The header declares the enabled
function and the `.cpp` implements it. The hook `.cpp` must be added to CMake
manually. If hook code breaks the build, the build should fail normally.

Example:

```cpp
// src/vkfwd/ferry/core/hook/vkCreateDeviceHook.hpp
template <>
struct CommandHooks<vkfwd::generated::CommandId::CreateDevice> {
  static constexpr bool before_pack_enabled = true;
  static constexpr bool after_pack_enabled = false;
  static constexpr bool before_unpack_enabled = false;
  static constexpr bool after_unpack_enabled = false;

  using Parameters = vkfwd::generated::commands::vkCreateDevice::Parameters;
  static void before_pack(Parameters& parameters);
};
```

## Handshake And Stream Compatibility

Before any command payloads are streamed, the dispatcher and receiver must
complete a handshake. The receiver/replay runtime may be newer or older than
the interceptor that produced a stream, so compatibility is decided once at
session setup, before command payload decoding starts.

The handshake includes:

- Magic value.
- Wire major and minor version.
- Vulkan API major, minor, and patch version that the generator was built
  against. Note that it could be different than the actual version that
  the game/app is using.
- Generator schema version.

Receiver rules:

- Reject bad magic.
- Reject unsupported wire major versions.
- Accept wire minor versions only within the receiver's readable range.
- Require Vulkan major version to match.
- Reject streams from a newer Vulkan minor version unless a deliberate adapter
  exists.
- Treat Vulkan patch/header differences as diagnostics unless a command policy
  requires stricter behavior.

After the handshake succeeds, command packets must not carry or revalidate the
stream version. The rest of the stream is interpreted according to the
negotiated version to keep the hot path small. Command-specific decoding may
still reject unknown command ids, unsupported payload revisions, missing
extensions, or unimplemented replay policies.

## Command Payload Versioning

The negotiated stream version is not enough by itself. Each command payload
should eventually carry a compact payload revision so one receiver can support
older shapes of the same command. Payload revisions are needed when generated
packing for a command changes layout while remaining wire-major compatible.

Rules:

- Additive payload changes can be handled by bumping a command payload revision
  and teaching unpack to fill defaults for older revisions.
- Breaking changes require a wire-major bump or an explicit compatibility
  adapter.
- Command ids must remain stable within a wire-major version.
- Removed or unsupported commands must produce explicit receiver errors, not
  silent no-ops.

## Per-Command Schema

For each Vulkan command, generation should produce a per-command schema file
that fully defines the `vkfwd` payload contract for that command. This schema
is more than the Vulkan C signature. It is the source of truth for the bytes
that pack/unpack code must produce and consume.

The schema should include Vulkan-facing facts:

- Command name and stable command id.
- Return type and result handling.
- Parameter order, names, C declarations, base types, constness, pointer depth,
  optionality, array length expressions, and string markers.
- Handle types, handle parent relationships, and dispatch level.
- Struct members and `pNext` eligibility from the pinned Vulkan XML.

The schema should also include `vkfwd` packing policy:

- Payload revision.
- Payload schema hash.
- Scalar encoding width and byte order.
- Shallow versus owned deep-copy policy for each pointer.
- Optional pointer presence encoding.
- Counted array encoding.
- String encoding.
- Handle encoding as source identity, not receiver handle values.
- Output/result policy.
- Supported `pNext` structures and unknown `pNext` behavior.
- Host memory payload capture timing where relevant.
- Replay mapping assumptions when they affect payload shape.

Generated C++ pack/unpack code must strictly follow the per-command schema.
The generator may build an in-memory model while parsing `vk.xml`, but the
checked-in generated artifacts should stay local: one small manifest plus
per-command schema/metadata files beside per-command generated code.

The current proof slice emits `.metadata.json` files beside the generated
command files. These are an early form of per-command metadata, not yet full
payload schemas. They should evolve into files such as:

```text
src/vkfwd/ferry/core/generated/command/vkCreateDevice.schema.json
```

## Schema Hash And Revision Enforcement

The generator should compute each command's schema hash during generation from
a canonical form of the per-command schema. This is done before runtime; the
hot path should never hash schemas.

Definitions:

- **Computed hash** is the hash the generator calculates from the command
  schema it is about to emit.
- **Policy hash** is the previously approved hash stored in a checked-in
  compatibility policy.

The compatibility policy is human-reviewed and manually updated because each
change is a compatibility decision. The generator may print the new computed
hash or produce a draft update, but it must not silently approve schema
changes.

Example policy shape:

```json
{
  "commands": {
    "vkCreateDevice": {
      "latest_revision": 3,
      "schema_hash": "sha256:...",
      "supported_revisions": [2, 3],
      "rejected_revisions": {
        "1": "shallow pointer payload was never replay-stable"
      }
    }
  }
}
```

Generator rules:

- Computed hash equals policy hash: schema is unchanged.
- Computed hash differs and revision is unchanged: fail generation.
- Computed hash differs and revision is bumped with policy updated: accept.
- Computed hash differs but a developer claims compatibility: require an
  explicit policy entry explaining the adapter/defaulting rule.
- Command schema changes caused by internal pack/unpack logic are treated the
  same as Vulkan XML changes if they change payload bytes.

Examples of internal changes that must affect the schema hash:

- `vkCreateDevice` changes from raw pointer capture to owned
  `VkDeviceCreateInfo` deep copy.
- `vkCreateDevice` adds support for selected `pNext` feature structs.
- Handle encoding changes from raw source handle values to source handle ids.
- Array or string encoding changes.
- Output/result replay policy changes.

The schema hash is a guardrail for generation and diagnostics. Runtime should
use compact command ids and payload revisions for normal dispatch.

## Receiver Multi-Revision Support

The receiver should generate revision-specific decoders and adapters for each
command. The public replay path should consume one current receiver-owned
record shape, while generated decode code adapts older supported revisions into
that latest shape.

Conceptual layout:

```text
src/vkfwd/ferry/core/generated/command/vkCreateDevice/
  schema.rev1.json
  schema.rev2.json
  schema.rev3.json
  decode_rev1.cpp
  decode_rev2.cpp
  decode_rev3.cpp
  adapters.cpp
  replay.cpp
```

The exact file layout can change, but the ownership model should not:
historical schemas that remain supported must stay checked in so the receiver
can regenerate older decoders and adapters.

Runtime flow:

```cpp
CommandEnvelope envelope = read_envelope();

switch (envelope.command_id) {
case CommandId::CreateDevice:
  return decode_vkCreateDevice(envelope);
}
```

Per-command decode then switches on payload revision:

```cpp
LatestCreateDeviceRecord decode_vkCreateDevice(const CommandEnvelope& e) {
  switch (e.payload_revision) {
    case 1:
      return adapt_rev1_to_latest(decode_rev1(e.payload));
    case 2:
      return adapt_rev2_to_latest(decode_rev2(e.payload));
    case 3:
      return decode_rev3(e.payload);
    default:
      reject_unsupported_revision(e.payload_revision);
  }
}
```

Generator responsibilities:

- Read all supported schema revisions for each command.
- Emit one decoder for each supported revision.
- Emit direct decode for the latest revision.
- Emit generated default adapters only when policy says the change is
  mechanically safe, such as a newly added field with a well-defined default.
- Emit adapter stubs that fail the build when human semantic logic is required.
- Emit explicit rejection paths for unsupported or retired revisions.

Receiver policy:

- Newer receiver reading older command revision: adapt if supported.
- Older receiver seeing newer command revision: reject unless it has an
  explicit forward-compatibility adapter or skip/default rule.
- Unknown command id: reject or route to an explicit unsupported-command
  policy.
- Never silently reinterpret an unknown revision as a known revision.

This lets a single receiver/replay runtime support many interceptor builds
without accepting ambiguous payloads or forcing the entire stream to be
rejected when only one command's payload changed.

## Ownership Rules

Packing must remove dependencies on application-owned memory before a payload
can outlive the intercepted call.

Generated pack code must own:

- Scalar values.
- Handles as source handle ids or source handle values tagged by type.
- Strings, including nullability.
- Counted arrays, including both count and element data.
- Nested structs.
- Known `pNext` chains by `sType`.
- Host memory payloads according to an explicit capture timing policy.

Generated pack code must not store borrowed pointers in replayable payloads
unless the record is documented as synchronous-only and cannot be queued or sent
to a receiver.

Unknown `pNext` structures must be one of:

- Rejected as unsupported.
- Ignored with a documented reason.
- Captured as opaque bytes only when the Vulkan contract and replay policy make
  that safe.

## Efficient Encoding Strategy

The preferred wire layout is a compact binary protocol with generated
command-specific encoders and decoders.

Recommended shape:

- One handshake per connection/file/chunk before command payload streaming.
- One fixed command envelope per call.
- Command id as a fixed-width integer.
- Sequence number as a fixed-width integer.
- Direction and ordering domain as compact enums.
- Payload byte size before command-specific bytes.
- Command-specific payload encoded by generated code.

For command payloads:

- Fixed-width scalars are copied directly in a canonical byte order.
- Enums and bitmasks use their Vulkan-defined storage width.
- Optional pointers use a presence bit or compact marker before data.
- Counted arrays encode count first, then contiguous element bytes or nested
  element encodings.
- Strings encode byte length plus bytes; avoid scanning on the receiver.
- Handles encode source identity and type, not receiver handle values.
- `pNext` chains encode a sequence of known `sType` records.

The encoder should prefer a contiguous append buffer with reserve estimates
from generated metadata. Avoid per-field heap allocation. For common small
commands, keep packed data trivially movable and cheap to construct.

## API Endpoint Boundary

The endpoint sits above transport and replay mechanics. It is the abstraction
interceptors call after packing a command, and a complete endpoint must behave
like the end of the Vulkan API call from the application's perspective.

Endpoint implementations may:

- Replay in-process against a local Vulkan driver.
- Send the command to a remote receiver and wait for the required response.
- Write capture/log data as a side effect.
- Use test doubles for deterministic local debugging.

Endpoint implementations must:

- Return the command's Vulkan-visible return value.
- Populate source-visible output parameters before returning.
- Create, resolve, and retire source-to-receiver handle mappings.
- Preserve command ordering and externally synchronized object assumptions.
- Surface receiver-side failure according to an explicit divergence policy.

Pure file, log, or queue writers are not top-level endpoints unless they also
provide the API-visible completion behavior above. They can still exist as
internal components of an endpoint.

## Replay Boundary

Unpack should produce receiver-owned data, but replay is a separate step.
Replay is where source handles are mapped to receiver handles and where Vulkan
calls are issued through receiver-side dispatch tables.

Unpack must not:

- Reuse source pointer values.
- Reuse source driver object identity as receiver identity.
- Mutate receiver Vulkan state before compatibility and payload validation
  succeed.

Replay must:

- Add handle mappings only after receiver-side creation succeeds.
- Remove mappings according to destruction policy.
- Preserve ordering and externally synchronized assumptions.
- Surface unsupported commands explicitly.

## Testing Requirements

The pack/unpack path needs tests at several levels:

- Generator determinism tests for generated pack/unpack files.
- Compile tests proving absent hooks add no calls or branches in generated code.
- Compile/link tests proving enabled hooks can live out of line in `.cpp`.
- Handshake compatibility tests proving version checks happen once before
  command streaming.
- Golden binary tests for payload layout.
- Round-trip tests for scalars, strings, arrays, handles, structs, and `pNext`.
- Compatibility tests for older readable wire minor versions.
- Negative tests for unknown commands, unsupported payload revisions, and
  unsupported `pNext` structures.

The current proof slice validates deterministic generation and an out-of-line
`vkCreateDevice` `before_pack` hook, but it does not yet validate binary
payload layout or deep-copy ownership.
