# Contributing

## Commenting Rule

Code comments in this repository should explain design intent, constraints, and
invariants. Prefer comments that answer "why is this boundary here?", "what
must remain true?", "what input shape is expected?", and "what behavior does the
caller rely on?".

Avoid comments that merely repeat the code. A comment such as "increment i" is
noise. A useful comment explains why the loop order matters, why a Vulkan
loader rule is being followed, why ownership is intentionally not transferred,
or why a placeholder is deliberately narrow.

For Vulkan interception and replay code, comments should call out:

- Loader-chain assumptions and dispatch-table invariants.
- Pointer, array, and `pNext` lifetime expectations.
- Pack/unpack ownership rules for borrowed application memory.
- Handle-mapping assumptions between source and receiver Vulkan instances.
- Replay ordering requirements and externally synchronized state.
- Placeholder behavior that must not be mistaken for complete forwarding.
