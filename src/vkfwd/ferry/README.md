# vkfwd ferry

`ferry` is the mechanical per-API-call forwarding implementation. It owns the
current Vulkan layer, generated command model, pack/unpack code,
receiver/replay scaffolding, generators, and tests for this approach.

All implementation-specific logic stays here for now: loader-chain setup,
instance/device dispatch tables, layer hooks, generated interceptors, transport
policy, schema-versioned command chunks, copied parameter storage, and receiver replay
helpers. Common code should move out only after another implementation has the
same invariants.

The implementation bias is to generate most Vulkan API handling and submit every
intercepted call to a transport channel. The forwarder does not call a local Vulkan
driver or keep per-instance/per-device state beyond shared dispatch tables. Any
local replay, remote transport, logging, response synthesis, or handle mapping
policy belongs behind the channel boundary.

The current generated slice is intentionally small: `vkCreateInstance`,
`vkDestroyInstance`, `vkCreateDevice`, and `vkDestroyDevice`. Its generated
body shape is hook, pack, channel send, unpack response, hook, return. The channel
response is still a placeholder until real command-response payloads and
source-to-receiver handle mapping are implemented.
