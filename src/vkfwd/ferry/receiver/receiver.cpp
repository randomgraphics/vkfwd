#include "receiver.hpp"

#include "generated/endpoints.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <limits>
#include <memory>

namespace vkfwd {
namespace {

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t & value) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) { return false; }
    value = lhs + rhs;
    return true;
}

class DispatchingApiResponder final : public ReceiverSession::ApiResponder {
public:
    explicit DispatchingApiResponder(receiver::ReplayContext & replay_context): replay_context_(replay_context) {}

    Blob receive_accumulated_api_calls(const Blob & request_blob) override {
        if (request_blob.size() < kSourceThreadIdSize) {
            VKFWD_LOG_ERROR("vkfwd receiver: request stream is missing source-thread prefix, size={}", request_blob.size());
            return {};
        }

        Blob response_blob;

        std::size_t offset = kSourceThreadIdSize;
        while (offset < request_blob.size()) {
            const auto header_view = request_blob.at<CommandChunkHeader>(offset);
            const auto header      = header_view.address();
            if (!header) {
                VKFWD_LOG_ERROR("vkfwd receiver: could not read command chunk header, offset={}, request_size={}", offset, request_blob.size());
                return {};
            }
            if (header->size < sizeof(CommandChunkHeader)) {
                VKFWD_LOG_ERROR("vkfwd receiver: invalid command chunk size, offset={}, command_id={}, size={}", offset, header->command_id, header->size);
                return {};
            }

            std::size_t next_offset = 0;
            if (!checked_add(offset, header->size, next_offset) || next_offset > request_blob.size()) {
                VKFWD_LOG_ERROR("vkfwd receiver: command chunk exceeds request stream, offset={}, command_id={}, size={}, request_size={}", offset,
                                header->command_id, header->size, request_blob.size());
                return {};
            }

            const auto         command_id = static_cast<generated::CommandId>(header->command_id);
            const CommandChunk request_packet {
                .command_offset = offset,
                .command_size   = header->size,
            };
            if (!receiver::generated::call_api_endpoint(command_id, request_blob, request_packet, response_blob, replay_context_)) {
                VKFWD_LOG_ERROR("vkfwd receiver: failed to dispatch API endpoint, command_id={}", header->command_id);
                return {};
            }

            offset = next_offset;
        }

        return response_blob;
    }

private:
    receiver::ReplayContext & replay_context_;
};

} // namespace

Receiver::Receiver(ReceiverSession & session, receiver::ReplayContext & replay_context): session_(session), replay_context_(replay_context) {
    // ReceiverSession owns source-thread demultiplexing. The factory gives each
    // source stream a responder whose mutable replay state can later diverge
    // without making Receiver know about session internals.
    session_.register_api_responder_factory([this] { return std::make_unique<DispatchingApiResponder>(replay_context_); });
}

} // namespace vkfwd
