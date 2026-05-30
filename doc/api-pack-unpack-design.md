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
stream even when it stores bytes in multiple internal chunks. `Blob::size()`
returns the current logical end offset and is the only public way to ask where
the next allocation will start.

The public `Blob` interface should stay deliberately small:

- `grow()` is the only append/allocate operation. It returns a bounded
  `SafeArrayView` over exactly the new bytes. Generated packers copy through
  that view instead of calling convenience append helpers.
- `data_at()` is the bounded read operation. It returns
  `SafeArrayView<const std::uint8_t>`, never a raw pointer, so unpack code keeps
  the offset and size contract visible until the explicit reinterpret boundary.
- `reset()`, `size()`, and `chunk_size()` are simple state queries/control.
- `Blob` must not expose random mutable lookup or overwrite APIs. If generated
  packing needs to patch a field, it must keep the typed pointer or typed view
  returned by `grow()` and write through that explicit slot.

This keeps `Blob` an arena plus bounded read view, not a generic mutable byte
buffer. The packer should know which Vulkan field it is patching at the patch
site.

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
padding required to align the payload
command argument shallow copy
referenced argument data
```

The header and shallow argument payload are allocated as one contiguous range.
The payload starts at a generated alignment-correct offset inside that range, so
unpack can safely reinterpret the payload without depending on the history of
`Blob::grow()` calls. Referenced argument data follows in later `grow()` ranges.

The stable command id identifies the Vulkan command. The size covers the fixed
command range: header, padding, and shallow argument payload. The per-command
payload revision selects the generated payload layout for that command under the
already-negotiated schema version. `CommandChunk` stores the command offset and
fixed command size; referenced data is reached through patched offsets in the
payload.

Command argument shallow copies keep scalar values directly. Pointer argument
slots are patched through explicitly retained typed pointers such as
`packed_parameters->pCreateInfo`; they are not patched by asking `Blob` to
overwrite an arbitrary byte offset. If a command argument points to a typed
Vulkan struct, the referenced data is a structure chunk. If it points to a plain
array, string array, allocation callback table, or non-typed struct, the
referenced data is stored according to generated metadata.

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

Before packing a `pNext` chain, generated code must validate the chain without
dumping it. Validation rejects loops, unreasonable depth (the current generated
limit is 1000 nodes), and unknown `sType` values. A failed `pNext` validation
fails the whole command pack so replay never receives a partial extension chain.

Structure packers must shallow-copy the typed struct into `Blob` with `grow()`
and retain the returned typed pointer, for example `packed_value`. Every pointer
member patch must write through the named Vulkan field on that typed pointer:
`packed_value->pNext`, `packed_value->pApplicationInfo`,
`packed_value->ppEnabledExtensionNames`, and so on. This rule intentionally
keeps pointer ownership and patch intent local to the generated code that knows
the Vulkan field semantics.

String-array payloads follow the same rule at the array level: allocate the
array of encoded pointer offsets as `SafeArrayView<std::uintptr_t>`, then patch
individual elements through that typed view after each string is copied.

## Pack/Unpack Shape

Generated command pack code should stay thin. It owns the command chunk header,
simple scalar fields, POD-like command arrays, and command-relative pointer
patching. When a command argument points at a typed Vulkan structure or typed
structure array, command code must call the generated helper under
`src/vkfwd/ferry/core/generated/structure/`; it must not duplicate
structure-member walking, `pNext` traversal, or structure-relative offset rules.

Generated pack code should be shaped like direct hand-written code:

1. Optionally call `before_pack` hooks with `if constexpr`.
2. Allocate one fixed command range with `Blob::grow()` for the command header,
   alignment padding, and shallow command argument record.
3. Fill the header and shallow command argument record through the returned
   view, retaining a typed pointer to the packed argument payload when later
   pointer patching is required.
4. Pack simple pointed-to command data into `Blob`, patching command-relative
   offsets through explicit typed payload fields.
5. Delegate typed structures and typed structure arrays to generated structure
   helpers, which own recursive structure packing and `pNext` traversal.
6. Optionally call `after_pack` hooks with `if constexpr`.

Generated unpack mirrors this:

1. Read the command id, command size, and command revision from the chunk
   header.
2. Validate the command id against the expected generated command.
3. Validate the command revision against supported generated layouts.
4. Read the shallow command argument record through `Blob::data_at()` and unwrap
   the returned safe view only at the explicit reinterpret boundary.
5. Resolve command-relative pointer offsets into receiver-owned views or copies.
6. Resolve structure-relative pointer offsets within each unpacked structure.
7. Reject unsupported `sType`, invalid offsets, and inconsistent count/pointer
   pairs before replay.
8. Hand receiver-owned data to replay or a replay adapter.

Unpack must not mutate receiver Vulkan state before payload validation and
handle mapping succeed.

## Generated Code Rules

Generated pack/unpack functions take `Blob&` as their first parameter. Packet
metadata is passed separately as a `CommandChunk&`; packets do not own or embed
the blob, command id, or copied parameters. Pack functions take the raw
parameter/response struct as the second argument and the output chunk as the
third argument. Unpack functions take the blob, the chunk, and the output
parameter/response struct.

Do not use nullable pointers for required generated outputs. Use references
when the caller must provide storage. Reserve pointer parameters for Vulkan API
data whose nullability is part of the Vulkan contract.

Error checks should mark the expected direction with `[[likely]]` or
`[[unlikely]]` in hot generated paths. The innermost failing function should log
the detailed root-cause message. Callers that merely propagate a `VkResult`
returned by another `vkfwd` helper should not log the same failure again.

Generated code must prefer direct typed operations over generic byte mutation:

- Append data with `Blob::grow()` and copy through the returned
  `SafeArrayView`.
- Read with `Blob::data_at()` and unwrap the const safe view only where the code
  validates and reinterprets the expected type.
- Patch copied pointer fields through explicit typed slots retained from
  `grow()`, never through a `Blob` overwrite by offset.
- Keep switch-based dispatch for generated `pNext` fast paths; use generic
  `sType` lookup only as the fallback for known types not covered by the switch.

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
