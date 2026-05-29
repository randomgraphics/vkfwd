# vkfwd ferry

`ferry` is the mechanical per-API-call forwarding implementation. It owns the
current Vulkan layer, generated command model, serialization/deserialization
code, receiver/replay scaffolding, generators, and tests for this approach.

All implementation-specific logic stays here for now: loader-chain setup,
instance/device dispatch tables, layer hooks, generated interceptors, endpoint
policy, local call buffers, and receiver replay helpers. Common code should move
out only after another implementation has the same invariants.

The implementation bias is to generate most Vulkan API handling, deep-copy
call data into local buffers, defer calls while the Vulkan-visible contract can
be satisfied locally, and flush to the remote endpoint when a return value,
output parameter, synchronization result, or remote handle mapping is required.
APIs with outputs are deferrable only when the output is locally computable,
cached, or covered by an explicit synthetic identity policy.
