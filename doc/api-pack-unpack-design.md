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

## Command Stream

`CommandStream` is the generated packer's output target. It is a logical byte
stream backed by one or more stable internal chunks. Logical offsets are
measured from the beginning of the stream, not from the current backing
allocation, and `CommandStream::size()` returns the current logical end offset.

The public `CommandStream` interface should stay deliberately small:

- `grow()` is the only append/allocate operation. It returns a bounded
  `SafeArrayView` over exactly the new bytes. Callers that need the logical byte
  offset request it through the optional offset out-parameter.
- `at()` is the bounded lookup operation. It returns a view only when the entire
  requested byte range is present in one backing chunk; unpack must treat an
  empty view as failed validation, not as a partial object.
- Typed `grow<T>()` and `at<T>()` still use byte offsets and byte sizes at the
  stream boundary. The typed overloads only add alignment, whole-object sizing,
  and `SafeArrayView<T>` reinterpretation after the byte range is proven valid.
- `flatten()` copies the logical stream into one backing allocation for
  transports or tests that need a single byte span. It preserves the logical
  bytes, including explicit gap records and zeroed padding.
- `reset()`, `size()`, and `is_contiguous()` are simple state queries/control.

This keeps `CommandStream` an arena plus bounded byte views, not a generic
mutable byte buffer. The packer should know which Vulkan field it is patching at
the patch site.

Pointers are never serialized as addresses:

- Every non-null pointer slot is encoded as a field-relative byte offset. The
  stored integer is the target stream offset minus the pointer field's own
  stream offset.
- Null pointers are encoded as a zero pointer value.
- Unpack repairs pointer fields in place by adding the encoded offset to the
  address of the pointer field inside the mutable stream-backed view.

This field-relative rule is an invariant: it lets command parameters, nested
structures, string arrays, and `pNext` chains be moved or flattened without
tracking a separate base for each record. The receiver must still validate that
the pointer slot and recovered target both fall inside the command or structure
view before exposing a repaired pointer.

## Alignment And Gaps

`CommandStream` alignment is both a logical stream rule and a physical backing
allocation rule. Generated packers request the natural alignment of each typed
payload, and command chunks request `CommandStream::kBaseAlignment` so chunk
headers are easy to discover after deferrable commands are accumulated.

Alignment requests are normalized to a power of two no larger than the base
alignment. A typed lookup is valid only when the byte offset is naturally
aligned for `T`, the requested size is at least `sizeof(T)`, and the size is an
integer multiple of `sizeof(T)`. Gap records or other unaligned protocol bytes
must be read as bytes and copied into a local object before interpretation.

When a new allocation cannot fit in the current backing chunk, or would leave a
tail too small to identify safely, `CommandStream` closes the current chunk with
a `CommandStreamGapHeader`. The gap header's `size` covers the whole remaining
logical range to skip. Any bytes after the header are semantically dead padding
and are zeroed so flattening or transport framing never exposes stale arena
contents.

Receivers and test helpers must discover commands by reading a valid
`CommandChunkHeader`, skip explicit gap records by their recorded size, and
treat plain alignment zero bytes as padding. They must not guess command ids from
padding or reinterpret a gap as a command.

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
The payload starts at a generated alignment-correct byte offset inside that
range, so unpack can safely reinterpret the payload after validating the
containing view. Referenced argument data follows in later `grow()` ranges.

The stable command id identifies the Vulkan command. The size initially covers
the fixed header, padding, and shallow argument payload. After referenced data is
packed, generated code finalizes the chunk by rewriting the header size to cover
the full serialized command range. The per-command payload revision selects the
generated payload layout for that command under the already-negotiated schema
version. `CommandChunk` stores the command offset and final command size;
referenced data is reached through field-relative offsets in the payload.

Command argument shallow copies keep scalar values directly, but shallow copies
are only an intermediate shell. Any copied pointer field that crosses the stream
boundary must be patched to null or to a field-relative offset before the chunk
is finalized. Pointer argument slots are patched through explicitly retained
typed pointers such as `packed_parameters->pCreateInfo`; they are not patched by
asking `CommandStream` to overwrite an arbitrary byte offset. If a command
argument points to a typed Vulkan struct, the referenced data is packed by the
structure helpers. If it points to a plain array, string array, allocation
callback table, or non-typed struct, the referenced data is stored according to
generated metadata.

Generated command code intentionally drops `VkAllocationCallbacks` by encoding
those slots as null. Allocation callbacks contain source-process function
pointers and user data, so replay must use the receiver's default allocator
unless a future receiver-local allocator policy is added.

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
data is found by reading known pointer/count fields and by following repaired
field-relative pointers, including `pNext`.

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

Structure packers must shallow-copy the typed struct into `CommandStream` with
typed `grow()` and retain the returned typed pointer, for example
`packed_value`. That shallow copy is valid only after every pointer member that
could contain a source-process address is patched through the named Vulkan field
on that typed pointer: `packed_value->pNext`,
`packed_value->pApplicationInfo`, `packed_value->ppEnabledExtensionNames`, and
so on. This rule intentionally keeps pointer ownership and patch intent local to
the generated code that knows the Vulkan field semantics.

String-array payloads follow the same rule at the array level: allocate the
array of encoded pointer offsets as `SafeArrayView<std::uintptr_t>`, then patch
individual elements through that typed view after each string is copied.

## Pack/Unpack Shape

Generated command pack code should stay thin. It owns the command chunk header,
simple scalar fields, POD-like command arrays, and field-relative pointer
patching. When a command argument points at a typed Vulkan structure or typed
structure array, command code must call the generated helper under
`src/vkfwd/ferry/core/generated/structure/`; it must not duplicate
structure-member walking, `pNext` traversal, or structure pointer rules.

Generated pack code should be shaped like direct hand-written code:

1. Optionally call `before_pack` hooks with `if constexpr`.
2. Allocate one fixed command range with `CommandStream::grow()` for the command
   header, alignment padding, and shallow command argument record.
3. Fill the header and shallow command argument record through the returned
   view, retaining a typed pointer to the packed argument payload when later
   pointer patching is required.
4. Pack simple pointed-to command data into `CommandStream`, patching
   field-relative offsets through explicit typed payload fields.
5. Delegate typed structures and typed structure arrays to generated structure
   helpers, which own recursive structure packing and `pNext` traversal.
6. Optionally call `after_pack` hooks with `if constexpr`.

Generated unpack mirrors this:

1. Read the command id, command size, and command revision from the chunk
   header.
2. Validate the command id against the expected generated command.
3. Validate the command revision against supported generated layouts.
4. Read the shallow command argument record from a mutable byte view and unwrap
   the returned safe view only at the explicit reinterpret boundary.
5. Resolve command pointer offsets in place after proving each slot and target
   are inside the command view.
6. Resolve structure pointer offsets within each unpacked structure after
   proving each slot and target are inside that structure's tail view.
7. Reject unsupported `sType`, invalid offsets, and inconsistent count/pointer
   pairs before replay.
8. Hand receiver-owned data to replay or a replay adapter.

Unpack must not mutate receiver Vulkan state before payload validation and
handle mapping succeed.

## Generated Code Rules

Generated pack/unpack functions take `CommandStream&` or a mutable
`SafeArrayView<std::uint8_t>&` as their stream-backed storage parameter. Packet
metadata is passed separately as a `CommandChunk&`; packets do not own or embed
the stream, command id, or copied parameters. Pack functions take the raw
parameter/response struct as the second argument and the output chunk as the
third argument. Unpack functions take the stream view, the chunk, and the output
parameter/response struct.

Do not use nullable pointers for required generated outputs. Use references
when the caller must provide storage. Reserve pointer parameters for Vulkan API
data whose nullability is part of the Vulkan contract.

Error checks should mark the expected direction with `[[likely]]` or
`[[unlikely]]` in hot generated paths. The innermost failing function should log
the detailed root-cause message. Callers that merely propagate a `VkResult`
returned by another `vkfwd` helper should not log the same failure again.

Generated code must prefer direct typed operations over generic byte mutation:

- Append data with `CommandStream::grow()` and copy through the returned
  `SafeArrayView`.
- Read with `CommandStream::at()` or a command-local `SafeArrayView` and unwrap
  only where the code validates and reinterprets the expected type.
- Patch copied pointer fields through explicit typed slots retained from
  `grow()`, never through a `CommandStream` overwrite by offset.
- Keep switch-based dispatch for generated `pNext` fast paths; use generic
  `sType` lookup only as the fallback for known types not covered by the switch.

## Compatibility

Command chunks carry command ids and payload revisions, but do not currently
have a concrete session-negotiation message. Future transports should negotiate
stream magic, schema version, and Vulkan API version once per session instead of
repeating schema compatibility data on every command.

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
- CommandStream tests for natural typed alignment, byte-offset typed access,
  bounded views, flattening, explicit gap records at chunk boundaries, and
  zeroed chunk-end padding.
- Negative tests for invalid offsets, inconsistent counts, unknown command ids,
  and unsupported `sType` values.
- Compile/link tests proving disabled hooks compile away and enabled hooks can
  live out of line.
