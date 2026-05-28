# Agent Instructions

## Commenting Rule

When editing this repository, add or preserve comments that explain design
intent, constraints, assumptions, invariants, and expected behavior. Comments
should answer why code is shaped a certain way, what must remain true, what
input/output contract matters, or what future implementation boundary is being
protected.

Do not add comments that merely restate the code. Prefer no comment over a
comment that only describes the immediate syntax.

For Vulkan interception, serialization, deserialization, and replay code,
explicitly document the reasoning around:

- Vulkan loader-chain and dispatch-table invariants.
- Pointer, array, and `pNext` lifetime assumptions.
- Ownership of copied parameter data and serialized payloads.
- Source-to-receiver handle mapping assumptions.
- Replay ordering, synchronization, and externally synchronized state.
- Placeholder behavior that should not be confused with complete forwarding or
  complete Vulkan replay.

This rule applies to agent-generated code and reviews as well as human-written
code. Keep comments concise, but bias toward preserving the engineering
rationale that would otherwise be lost.
