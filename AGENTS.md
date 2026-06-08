# Agent Instructions

## Testing Rule

Use `cit` for repository tests. Do not run `ctest` in this repository.

## Commenting Rule

When editing this repository, add or preserve comments that explain design
intent, constraints, assumptions, invariants, and expected behavior. Comments
should answer why code is shaped a certain way, what must remain true, what
input/output contract matters, or what future implementation boundary is being
protected, and what caller-visible behavior depends on the implementation
choice.

Do not add comments that merely restate the code. Prefer no comment over a
comment that only describes the immediate syntax. A useful comment explains why
a boundary exists, why loop order matters, why a Vulkan loader rule is being
followed, why ownership is intentionally not transferred, or why a placeholder
is deliberately narrow.

For Vulkan interception, serialization, deserialization, and replay code,
explicitly document the reasoning around:

- Vulkan loader-chain and dispatch-table invariants.
- Pointer, array, and `pNext` lifetime assumptions.
- Ownership of copied parameter data, serialized payloads, and borrowed
  application memory during pack/unpack operations.
- Source-to-receiver handle mapping assumptions.
- Replay ordering, synchronization, and externally synchronized state.
- Placeholder behavior that should not be confused with complete forwarding or
  complete Vulkan replay.

This rule applies to agent-generated code and reviews as well as human-written
code. Keep comments concise, but bias toward preserving the engineering
rationale that would otherwise be lost.

## Ferry Module Guidance

Before editing `src/vkfwd/ferry/`, read `src/vkfwd/ferry/README.md`. For
module-specific changes, also read the local README for the area you touch:

- `src/vkfwd/ferry/core/README.md` for protocol, blob, transport, generated
  command/structure serialization, hooks, and shared tests.
- `src/vkfwd/ferry/forwarder/README.md` for Vulkan layer entry points,
  generated forwarding wrappers, dispatch-table exposure, and transport-channel
  flushing behavior.

Keep new code aligned with those module contracts. If the contract has changed,
update the relevant README in the same change as the code.

Periodically refresh documentation so it matches the code, especially after
major design or module-boundary changes. When a change alters forwarding,
serialization, transport, replay, generated-code layout, or testing policy,
review the nearby README files and update stale guidance before finishing.
