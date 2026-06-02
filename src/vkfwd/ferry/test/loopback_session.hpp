#pragma once

#include "receiver_session.hpp"
#include "transport_session.hpp"

#include <memory>

namespace vkfwd {

struct LoopbackSession {
    std::shared_ptr<TransportSession> transport;
    std::unique_ptr<ReceiverSession>  receiver;

    // Creates paired in-process transport and receiver sessions for tests. The
    // transport flattens each source-thread request stream before handing it to
    // the receiver, preserving the real forwarding boundary without requiring a
    // socket, process, or platform IPC backend.
    static LoopbackSession create();
};

} // namespace vkfwd
