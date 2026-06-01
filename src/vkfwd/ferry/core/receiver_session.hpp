#pragma once

#include "blob.hpp"

namespace vkfwd {

struct ReceiverSession {
    virtual ~ReceiverSession() = default;

    struct ApiResponder {
        virtual ~ApiResponder() = default;

        // A receiver session is only the transport-to-replay boundary: it accepts one
        // accumulated source-thread stream and returns the matching response blob.
        virtual Blob receive_accumulated_api_calls(Blob && request_blob) = 0;
    };

    typedef std::function<std::unique_ptr<ApiResponder>()> ApiResponder;

    virtual void register_api_responder_factory(...) = 0;
};

} // namespace vkfwd
