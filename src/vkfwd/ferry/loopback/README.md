# vkfwd ferry loopback

`loopback` is an in-process simulation surface for the ferry pipeline. It binds
the real forwarder to a local `TransportSession` implementation that returns a
flattened copy of each request blob instead of delivering bytes to a receiver.

The module intentionally skips receiver replay for now. Its purpose is to prove
that generated Vulkan layer entry points, thread-local `Forwarder` state, shared
transport-session configuration, and blob flattening can run end to end inside
one process.

The loopback transport is stateless after construction. Multiple source threads
share one session, but each `send_accumulated_api_calls()` call only reads the
caller-owned request blob and returns an independent flattened response blob.
That keeps source-thread streams multiplexed without a transport-side lock or
cross-thread queue.
