#include "forwarder.hpp"

#include "logging.hpp"

#include <atomic>
#include <cassert>
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
    // ordering is represented by the source-thread token prefixed into each
    // accumulated request stream, not by separate per-thread transport objects.
    auto & session = shared_transport_slot();
    if (!session) { session = transport_creator_slot()(); }
    if (!session) { session = make_unconfigured_transport(); }
    return session;
}

SourceThreadId next_source_thread_id() {
    static std::atomic<SourceThreadId> next {1};
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

Forwarder::Forwarder(): transport_(shared_transport_instance()), thread_id_(next_source_thread_id()) { begin_request_stream(); }

void Forwarder::begin_request_stream() {
    // The receiver demultiplexes accumulated streams from one shared transport
    // session by reading this fixed-width prefix before any command chunk bytes.
    request_stream_.grow<SourceThreadId>() = thread_id_;
}

CommandStream Forwarder::flush() {
    assert(thread_id_ != 0);
    if (request_stream_.size() == 0) { begin_request_stream(); }
    // Configuration normally happens before application threads enter Vulkan,
    // but in-process tests replace the transport between cases. Refreshing at
    // the flush boundary keeps existing thread-local Forwarders aligned with
    // the current process-wide session without moving transport policy into
    // generated entry points.
    transport_                    = shared_transport_instance();
    CommandStream response_stream = transport_ ? transport_->send_accumulated_api_calls(request_stream_) : CommandStream {};
    request_stream_.reset();
    begin_request_stream();
    return response_stream;
}

} // namespace vkfwd
