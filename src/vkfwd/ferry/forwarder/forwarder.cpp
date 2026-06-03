#include "forwarder.hpp"

#include "logging.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace vkfwd {
namespace {

class UnconfiguredTransportSession final : public TransportSession {
public:
    CommandStream send_accumulated_api_calls(CommandStream & request_stream) override {
        // This placeholder keeps layer bring-up deterministic when no real
        // transport has been installed. Log at the transport boundary so the
        // later empty-response unpack failure does not hide the actual setup
        // problem, while still avoiding fake local Vulkan replay.
        if (!reported_.exchange(true, std::memory_order_relaxed)) {
            VKFWD_LOG_ERROR("vkfwd forwarder transport is not configured; dropping accumulated API stream, size={}", request_stream.size());
        }
        return {};
    }

private:
    std::atomic_bool reported_ {false};
};

std::shared_ptr<TransportSession> make_unconfigured_transport() { return std::make_shared<UnconfiguredTransportSession>(); }

std::shared_ptr<TransportSession> & shared_transport_slot() {
    // The process-level session is shared by every thread-local Forwarder, but
    // tests and startup code must be able to replace it before replay begins.
    // Keeping the slot separate from the creator lets configuration invalidate
    // an already-created placeholder transport.
    static std::shared_ptr<TransportSession> session;
    return session;
}

Forwarder::TransportCreator & transport_creator_slot() {
    // This process-level creator is configuration, not hot forwarding state. Set
    // it during layer/test startup before application threads enter Vulkan.
    static Forwarder::TransportCreator creator = make_unconfigured_transport;
    return creator;
}

std::shared_ptr<TransportSession> shared_transport_instance() {
    // All thread-local forwarders share the negotiated session. Per-thread
    // ordering is represented by the RequestStreamHeader embedded in each accumulated
    // request stream, not by separate per-thread transport objects.
    auto & session = shared_transport_slot();
    if (!session) { session = transport_creator_slot()(); }
    if (!session) { session = make_unconfigured_transport(); }
    return session;
}

StreamId next_stream_id() {
    static std::atomic<StreamId> next {1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

Forwarder & Forwarder::instance() {
    thread_local Forwarder forwarder;
    return forwarder;
}

void Forwarder::set_transport_creator(TransportCreator creator) {
    transport_creator_slot() = creator ? std::move(creator) : make_unconfigured_transport;
    shared_transport_slot().reset();
}

Forwarder::Forwarder(): stream_id_(next_stream_id()), transport_(shared_transport_instance()) { reset_request_stream(); }

void Forwarder::reset_request_stream() {
    request_stream_.reset();
    // Request routing is protocol framing, not CommandStream storage behavior.
    // Rewriting it here keeps response streams empty while preserving the
    // per-source-thread FIFO identity expected by transports and receivers.
    RequestStreamHeader header {
        .stream_id = stream_id_,
    };
    request_stream_.grow<RequestStreamHeader>(1, alignof(RequestStreamHeader)).set(0, header);
}

CommandStream Forwarder::flush() {
    // Configuration normally happens before application threads enter Vulkan,
    // but in-process tests replace the transport between cases. Refreshing at
    // the flush boundary keeps existing thread-local Forwarders aligned with
    // the current process-wide session without moving transport policy into
    // generated entry points.
    transport_ = shared_transport_instance();
    if (!transport_) {
        VKFWD_LOG_ERROR("vkfwd forwarder transport creator failed to produce a session; dropping accumulated API stream, size={}", request_stream_.size());
        reset_request_stream();
        return {};
    }
    auto response_stream = transport_->send_accumulated_api_calls(request_stream_);
    reset_request_stream();
    return response_stream;
}

} // namespace vkfwd
