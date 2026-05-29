# TODO

This file tracks known follow-up work that is intentionally not complete yet.
Items here should be revisited before broad Vulkan API generation or replay
support is treated as production-ready.

## Per-Command Payload Schema And Revision Management

The current generated per-command `*.metadata.json` files are metadata snapshots
for inspection and tests. They are not yet authoritative payload schemas.

Follow-up work:

- Define a dedicated per-command `payload_schema` format.
- Keep the metadata document schema version separate from command payload
  revision to avoid ambiguity.
- Generate a canonical payload schema for each command from Vulkan XML plus
  local pack/unpack policy.
- Include pack/unpack policy in that schema: ownership, deep-copy behavior,
  pointer/nullability encoding, array/string encoding, handle encoding,
  result/output behavior, and `pNext` policy.
- Compute a schema hash from only the canonical payload schema, not from the
  entire metadata document.
- Add a checked-in human-reviewed compatibility policy containing approved
  payload revisions and schema hashes.
- Fail generation when a computed schema hash changes without an explicit
  revision or compatibility-policy decision.
- Generate receiver-side multi-revision decoders and adapter stubs from the
  supported schema history.
- Decide final filenames, likely evolving current `*.metadata.json` files into
  explicit `*.schema.json` files once payload schemas are real.

Until this is implemented, command payload revision/hash management remains a
design requirement, not an enforced compatibility mechanism.

## Output Parameters, Return Values, And Handle Mapping

The replay protocol needs an explicit contract for commands whose observable
result is not only the forwarded input payload. Vulkan creation, enumeration,
query, memory-mapping, and synchronization calls often combine return values,
output pointers, caller-provided output arrays, and newly created handles.

Follow-up work:

- Decide whether return-value encoding and source-visible output-parameter
  population belong alongside pack/unpack in `core/generated/` and
  `core/hook/`. The current intuition is yes, because response payloads must
  follow the same command-specific schema, ownership, and compatibility rules as
  request payloads; however, this needs a deliberate boundary decision against
  the API endpoint and replay layers before implementation.
- Define how `VkResult` and non-`VkResult` return values are represented on the
  wire, including whether receiver-side failures are propagated back to the
  source application, converted to local failures, or treated as fatal replay
  divergence.
- Define ownership and lifetime rules for output parameters. The design must
  distinguish source-visible outputs that need to be written back locally from
  receiver-only outputs that only seed replay state.
- Define enumeration/query behavior for two-call count-and-array patterns,
  partial success, caller buffer truncation, and commands that mutate output
  structs through `pNext` chains.
- Define the source-to-receiver handle map as a first-class replay structure.
  Forwarded payloads should carry typed source handle identities, while replay
  resolves them to receiver-side handles created in the corresponding replay
  order.
- Define when handle mappings are inserted, updated, invalidated, and destroyed,
  especially for failed creation calls, externally synchronized object lifetime,
  dispatchable parent relationships, and commands that return multiple handles.
- Define the local-debug endpoint behavior separately from remote replay. Local
  shortcuts must not rely on raw source handles or process-local output pointer
  lifetimes in ways that would hide bugs in the remote protocol.
