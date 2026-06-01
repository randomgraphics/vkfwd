#pragma once

#include "blob.hpp"

#include <functional>
#include <memory>

namespace vkfwd {

class ReceiverSession {
public:
    virtual ~ReceiverSession() = default;

    class ApiResponder {
    public:
        virtual ~ApiResponder() = default;

        // A receiver session is only the transport-to-replay boundary: it
        // inspects one accumulated source-thread stream and returns the matching
        // response blob. The request remains transport-owned so responders must
        // copy anything they need after this call returns.
        virtual Blob receive_accumulated_api_calls(const Blob & request_blob) = 0;
    };

    using ApiResponderFactory = std::function<std::unique_ptr<ApiResponder>()>;

    // The session owns source-thread routing and responder lifetime. Receiver
    // registers a factory so each source-thread stream can get isolated replay
    // state without exposing the session's demultiplexing internals.
    virtual void register_api_responder_factory(ApiResponderFactory factory) = 0;
};

} // namespace vkfwd
