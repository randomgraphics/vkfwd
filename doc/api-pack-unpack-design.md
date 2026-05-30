# API Pack/Unpack Design

This document describes the generated binary pack/unpack path for intercepted
Vulkan API calls. The design goal is a moveable command stream: every borrowed
application pointer is replaced by an offset into `vkfwd`-owned bytes before a
command crosses the endpoint boundary.

The hot path should remain generated, direct, and schema-driven. Generic
reflection, string dispatch, and field-by-field virtual calls do not belong in
command packing or replay unpacking.

## Goals

- Generate command pack/unpack code from the pinned Vulkan XML.
- Generate structure pack/unpack helpers for Vulkan typed structs.
- Preserve enough parameter data to replay after application-owned memory is
  gone.
- Encode command chunks so workers can skip or unpack commands in parallel.
- Keep command ids stable across compatible Vulkan registry changes.
- Keep struct recognition based on Vulkan `sType`.
- Preserve explicit ownership, handle-mapping, replay-ordering, and unsupported
  behavior in generated code comments and schemas.

## Non-Goals

- Reusing source process pointer values on the receiver.
- Treating source driver handles as receiver driver handles.
- A self-describing text protocol.
- Complete Vulkan replay before ownership, ordering, and compatibility rules are
  correct.

## Generated Layout

Generated command code lives under:

```text
src/vkfwd/ferry/core/generated/command/
```

Generated Vulkan structure helpers live under:

```text
src/vkfwd/ferry/core/generated/structure/
```

The structure folder mirrors Vulkan's public header split. Core Vulkan structs
belong in core files. Platform-specific structs belong in matching
platform-specific generated files guarded by the same preprocessor symbols used
by the Vulkan headers.

Forwarder-specific entry points, dispatch lookup tables, and loader glue remain
under:

```text
src/vkfwd/ferry/forwarder/generated/
```

Manual hooks live outside generated output and must never be created,
overwritten, deleted, or disabled by generation:

```text
src/vkfwd/ferry/core/hook/<api>Hook.hpp
src/vkfwd/ferry/core/hook/<api>Hook.cpp
```

## Blob Stream

`Blob` is the generated packer's output target. It is a logical contiguous byte
stream even when it stores bytes in multiple internal chunks. `Blob::next_offset`
returns the effective offset of the next vacant byte from the beginning of the
logical stream. Command packers use this to remember the command start and patch
the final command size after all referenced payload bytes are written.

Pointers are never serialized as addresses:

- Pointers in command arguments are offsets from the beginning of the command
  chunk, where the stable command id is stored.
- Pointers inside a packed structure are offsets from the beginning of that
  structure chunk, where the structure `sType` is stored.
- Pointers inside a nested structure remain relative to that nested structure,
  not to the outer command or parent structure.
- Null pointers are encoded as zero offsets. Generated packers must place
  referenced data after the fixed command/struct body so a real target offset is
  never zero.

This local-offset rule is an invariant: it keeps each typed struct independently
moveable as a subrecord and keeps each command chunk independently skippable.

## Command Chunk

Each command chunk starts with a fixed header:

```text
stable command id
total command chunk size
per-command payload revision
command argument shallow copy
referenced argument data
```

The stable command id identifies the Vulkan command. The size covers the entire
chunk from the command id through the last referenced byte. The per-command
payload revision selects the generated payload layout for that command under the
already-negotiated schema version. Size exists at the command layer so a
receiver can skip unknown/unsupported commands or hand independent command
chunks to a pool of unpack workers when ordering policy allows it.

Command argument shallow copies keep scalar values directly. Pointer argument
slots are patched to command-relative offsets. If a command argument points to a
typed Vulkan struct, the referenced data is a structure chunk. If it points to a
plain array, string array, allocation callback table, or non-typed struct, the
referenced data is stored as a sized blob according to generated metadata.

## Structure Chunk

Vulkan typed structures are recognized by their `sType`, which is the first
field in the C struct. Each packed structure chunk is:

```text
VkStructureType sType
shallow copy of the struct bytes after sType
referenced member data
```

The structure layer does not carry a per-struct size. Its fixed body size and
member interpretation come from the Vulkan header and generated schema. Variable
data is found by reading known pointer/count fields and by following `pNext`.

For pointer members:

- A member that points to another typed Vulkan struct or typed struct array is
  packed recursively with the same structure-chunk format.
- A member that points to a plain scalar array or simple non-typed struct array
  is packed as a sized byte blob.
- `pNext` is always treated as a typed-struct chain. The first four bytes are
  read as `VkStructureType`; the matching generated helper packs the node; then
  the process repeats until `pNext` is null.

Unsupported `sType` values must be rejected explicitly. Opaque pNext copying is
not replay-stable unless a command-specific policy proves that the bytes contain
no process-local pointers and that replay can safely ignore the struct meaning.

## Pack/Unpack Shape

Generated command pack code should stay thin. It owns the command chunk header,
simple scalar fields, POD-like command arrays, and command-relative pointer
patching. When a command argument points at a typed Vulkan structure or typed
structure array, command code must call the generated helper under
`src/vkfwd/ferry/core/generated/structure/`; it must not duplicate
structure-member walking, `pNext` traversal, or structure-relative offset rules.

Generated pack code should be shaped like direct hand-written code:

1. Optionally call `before_pack` hooks with `if constexpr`.
2. Append a placeholder command header to `Blob`.
3. Append a shallow command argument record.
4. Pack simple pointed-to command data into `Blob`, patching command-relative
   offsets.
5. Delegate typed structures and typed structure arrays to generated structure
   helpers, which own recursive structure packing and `pNext` traversal.
6. Patch the command header with stable command id, total command chunk size,
   and per-command payload revision.
7. Optionally call `after_pack` hooks with `if constexpr`.

Generated unpack mirrors this:

1. Read the command id, command size, and command revision from the chunk
   header.
2. Validate the command id against the expected generated command.
3. Validate the command revision against supported generated layouts.
4. Read the shallow command argument record.
5. Resolve command-relative pointer offsets into receiver-owned views or copies.
6. Resolve structure-relative pointer offsets within each unpacked structure.
7. Reject unsupported `sType`, invalid offsets, and inconsistent count/pointer
   pairs before replay.
8. Hand receiver-owned data to replay or a replay adapter.

Unpack must not mutate receiver Vulkan state before payload validation and
handle mapping succeed.

## Compatibility

Before command chunks cross a transport, peers complete a handshake containing
the stream magic, schema version, and Vulkan API version used for generation.
After a successful handshake, command chunks do not repeat the schema version;
the hot path relies on the negotiated session.

Command ids must remain stable within a schema version. Payload layout changes
require schema revision policy. Unknown command ids, unsupported payload
revisions, and unsupported `sType` values must produce explicit errors.

## Replay Boundary

Packing and unpacking only establish owned data. Replay is separate and owns:

- Mapping source handles to receiver handles.
- Creating mappings only after receiver-side creation succeeds.
- Retiring mappings on destruction.
- Preserving command ordering, synchronization, and externally synchronized
  Vulkan object assumptions.
- Surfacing unsupported commands or divergence according to policy.

Placeholder endpoint behavior must be documented as placeholder behavior. It
must not be confused with complete forwarding or complete Vulkan replay.

## Testing Requirements

- Generator determinism tests for command and structure helpers.
- Golden binary tests for command headers, sizes, offsets, strings, arrays, and
  `pNext` chains.
- Round-trip tests for `vkCreateInstance` and `vkCreateDevice`.
- Negative tests for invalid offsets, inconsistent counts, unknown command ids,
  and unsupported `sType` values.
- Compile/link tests proving disabled hooks compile away and enabled hooks can
  live out of line.
