# TODO

This file tracks known follow-up work that is intentionally not complete yet.
Items here should be revisited before broad Vulkan API generation or replay
support is treated as production-ready.

## Revisit command stream versioning.

We need to revisit our version and forward/backward compatibility plan. The old
version system is incomplete. We need both backward and forward compatibility.
It means newer forward need to be able to work with slightly older receiver and
vise verse. Of course we need to maintain an supported revision list. any thing
outside of that, forward and receiver will refuse to communicate with each other
and cause the very first call (such as vkCreateInstance()) to fail.

Ideally, we'd like to have the version control granularity at per-api level. So
each api can involve independently. But if that gets over complicated, we could
fallback to simpler single version. This needs further discussion.

Also, imagine we have an newer forwarder and older receiver, with only one API logic is revised in forwarder. do we bump up the entire command stream version
for this single api change. If not, then how does the forwarder know that the
recever is still on older verion for this particular API.
