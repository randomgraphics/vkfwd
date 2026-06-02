#include "loopback_session.hpp"

#include "logging.hpp"

#include <utility>

namespace vkfwd {
namespace {

class LoopbackReceiverSession final : public ReceiverSession {
public:
    void register_api_responder_factory(ApiResponderFactory factory) override { factory_ = std::move(factory); }

    Blob receive_accumulated_api_calls(const Blob & request_blob) {
        if (!responder_) {
            if (!factory_) {
                VKFWD_LOG_ERROR("vkfwd loopback receiver has no API responder factory");
                return {};
            }
            responder_ = factory_();
        }
        if (!responder_) {
            VKFWD_LOG_ERROR("vkfwd loopback receiver factory returned no API responder");
            return {};
        }
        return responder_->receive_accumulated_api_calls(request_blob);
    }

private:
    ApiResponderFactory            factory_;
    std::unique_ptr<ApiResponder> responder_;
};

class LoopbackTransportSession final : public TransportSession {
public:
    explicit LoopbackTransportSession(LoopbackReceiverSession & receiver): receiver_(receiver) {}

    Blob send_accumulated_api_calls(Blob & request) override {
        // The receiver side should observe transport-owned contiguous bytes, not
        // the forwarder's mutable arena chunks. Flattening here makes loopback
        // exercise the same request lifetime boundary as a real backend.
        Blob flattened = request.flatten();
        return receiver_.receive_accumulated_api_calls(flattened);
    }

private:
    LoopbackReceiverSession & receiver_;
};

} // namespace

LoopbackSession LoopbackSession::create() {
    LoopbackSession session;
    auto receiver = std::make_unique<LoopbackReceiverSession>();
    auto transport = std::make_shared<LoopbackTransportSession>(*receiver);
    session.receiver = std::move(receiver);
    session.transport = std::move(transport);
    return session;
}

} // namespace vkfwd
