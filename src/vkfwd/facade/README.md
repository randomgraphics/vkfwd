# vkfwd facade

`facade` is reserved for the stateful Vulkan front-end implementation. It will
own local Vulkan-facing state and local handle identities when that approach is
ready, instead of sharing the per-API-call forwarding assumptions used by
`ferry`.

