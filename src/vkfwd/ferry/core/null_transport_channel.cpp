#include "null_transport_channel.hpp"

#include <cstdio>

namespace vkfwd {

Blob NullTransportChannel::send(Blob & request_blob) {
    // This channel is a trace-only placeholder. A real channel owns the
    // transport/replay details below this boundary and must produce the
    // Vulkan-visible response blob for the last command in the flushed stream.
    std::fprintf(stderr, "vkfwd: sent %zu request bytes\n", request_blob.size());
    return Blob {};
}

} // namespace vkfwd
