# vkfwd ferry tests

`test` holds in-process ferry integration tests and their local transport
helpers. The loopback session binds the real forwarder to a paired
`TransportSession` and `ReceiverSession`, so tests can exercise forwarding,
request flattening, receiver dispatch, and response unpacking without a socket,
process boundary, or platform IPC backend.

The current hello-world test is intentionally small. It documents the expected
end-to-end wiring around `vkCreateInstance`, while complete replay still depends
on generated command unpacking rehydrating serialized offsets into
receiver-side pointers before calling Vulkan.

The loopback transport shares one session with the forwarder because forwarding
state is process-wide and stream identity is carried in the request
stream header. Each `send_accumulated_api_calls()` call still flattens the
caller-owned request stream before handing it to the receiver, preserving the
transport lifetime boundary that real backends must honor.
