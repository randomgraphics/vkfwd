#pragma once

#include "blob.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

namespace vkfwd {

class AccumulatedApiCallResponder {
public:
    virtual ~AccumulatedApiCallResponder() = default;

    // Responders own per-source-thread replay state. The stream still includes
    // the fixed source-thread prefix so lower layers can retain byte-for-byte
    // diagnostics and command offsets that match the sender-side blob.
    virtual Blob process_accumulated_api_calls(Blob && request_blob) = 0;
};

class ReceiverSession {
public:
    using ResponderFactory = std::function<std::unique_ptr<AccumulatedApiCallResponder>(SourceThreadId)>;

    ReceiverSession() = default;
    explicit ReceiverSession(ResponderFactory factory): responder_factory_(std::move(factory)) {}

    void register_accumulated_api_handler(ResponderFactory factory) { responder_factory_ = std::move(factory); }

    Blob receive_accumulated_api_calls(Blob && request_blob) {
        if (request_blob.size() < kSourceThreadIdSize) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd receiver session rejected truncated accumulated API stream, size={}", request_blob.size());
            return {};
        }

        const auto thread_id_view = request_blob.at<SourceThreadId>(kSourceThreadIdOffset, 1);
        const auto * thread_id    = thread_id_view.empty() ? nullptr : &thread_id_view.at(0);
        if (!thread_id) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd receiver session could not read source-thread id from accumulated API stream");
            return {};
        }

        AccumulatedApiCallResponder * responder = get_responder(*thread_id);
        if (!responder) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd receiver session has no responder for source_thread_id={}", *thread_id);
            return {};
        }

        return responder->process_accumulated_api_calls(std::move(request_blob));
    }

private:
    AccumulatedApiCallResponder * get_responder(SourceThreadId thread_id) {
        if (auto existing = responders_.find(thread_id); existing != responders_.end()) { return existing->second.get(); }
        if (!responder_factory_) { return nullptr; }

        std::unique_ptr<AccumulatedApiCallResponder> responder = responder_factory_(thread_id);
        if (!responder) { return nullptr; }

        auto [entry, inserted] = responders_.emplace(thread_id, std::move(responder));
        return inserted ? entry->second.get() : nullptr;
    }

    ResponderFactory responder_factory_;

    // The source-thread prefix defines the per-thread replay boundary explicitly,
    // so the receiver keeps one responder per token while the transport remains
    // a shared session.
    std::unordered_map<SourceThreadId, std::unique_ptr<AccumulatedApiCallResponder>> responders_;
};

} // namespace vkfwd
