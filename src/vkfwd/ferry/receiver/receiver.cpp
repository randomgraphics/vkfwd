#include "receiver.hpp"

#include "generated/endpoints.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>

namespace vkfwd {
namespace {

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t & value) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) { return false; }
    value = lhs + rhs;
    return true;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    const std::size_t remainder = value % alignment;
    if (remainder == 0) { return value; }
    return value + (alignment - remainder);
}

bool read_gap_header(const CommandStream & stream, std::size_t offset, CommandStreamGapHeader & gap) {
    const auto view = stream.at(offset, sizeof(CommandStreamGapHeader));
    if (view.empty()) { return false; }
    std::memcpy(&gap, view.address(0), sizeof(gap));
    return gap.magic == kCommandStreamGapMagic;
}

class DispatchingApiResponder final : public ReceiverSession::ApiResponder {
public:
    explicit DispatchingApiResponder(receiver::ReplayContext & replay_context): replay_context_(replay_context) {}

    CommandStream receive_accumulated_api_calls(const CommandStream & request_stream) override {
        if (request_stream.size() < sizeof(CommandStream::StreamHeader)) {
            VKFWD_LOG_ERROR("vkfwd receiver: request stream is missing stream header, size={}", request_stream.size());
            return {};
        }
        const auto header_view = request_stream.at<CommandStream::StreamHeader>(0);
        const auto header      = header_view.address();
        if (!header || header->magic != CommandStream::StreamHeader {}.magic || header->revision != CommandStream::StreamHeader {}.revision) {
            VKFWD_LOG_ERROR("vkfwd receiver: request stream has invalid header");
            return {};
        }

        CommandStream response_stream;

        std::size_t offset = sizeof(CommandStream::StreamHeader);
        while (offset < request_stream.size()) {
            while (offset < request_stream.size()) {
                CommandStreamGapHeader gap {};
                if (read_gap_header(request_stream, offset, gap)) {
                    if (gap.size < sizeof(CommandStreamGapHeader)) {
                        VKFWD_LOG_ERROR("vkfwd receiver: invalid stream gap size, offset={}, size={}", offset, gap.size);
                        return {};
                    }
                    std::size_t next_offset = 0;
                    if (!checked_add(offset, gap.size, next_offset) || next_offset > request_stream.size()) {
                        VKFWD_LOG_ERROR("vkfwd receiver: stream gap exceeds request stream, offset={}, size={}, request_size={}", offset, gap.size,
                                        request_stream.size());
                        return {};
                    }
                    offset = next_offset;
                    continue;
                }

                const std::size_t aligned = align_up(offset, CommandStream::kBaseAlignment);
                if (aligned != offset) {
                    offset = aligned;
                    continue;
                }
                break;
            }
            if (offset >= request_stream.size()) { break; }

            const auto header_view = request_stream.at<CommandChunkHeader>(offset);
            const auto header      = header_view.address();
            if (!header) {
                VKFWD_LOG_ERROR("vkfwd receiver: could not read command chunk header, offset={}, request_size={}", offset, request_stream.size());
                return {};
            }
            if (header->size < sizeof(CommandChunkHeader)) {
                VKFWD_LOG_ERROR("vkfwd receiver: invalid command chunk size, offset={}, command_id={}, size={}", offset, header->command_id, header->size);
                return {};
            }

            std::size_t next_offset = 0;
            if (!checked_add(offset, header->size, next_offset) || next_offset > request_stream.size()) {
                VKFWD_LOG_ERROR("vkfwd receiver: command chunk exceeds request stream, offset={}, command_id={}, size={}, request_size={}", offset,
                                header->command_id, header->size, request_stream.size());
                return {};
            }

            const auto         command_id = static_cast<generated::CommandId>(header->command_id);
            const CommandChunk request_packet {
                .command_offset = offset,
                .command_size   = header->size,
            };
            if (!receiver::generated::call_api_endpoint(command_id, request_stream, request_packet, response_stream, replay_context_)) {
                VKFWD_LOG_ERROR("vkfwd receiver: failed to dispatch API endpoint, command_id={}", header->command_id);
                return {};
            }

            offset = next_offset;
        }

        return response_stream;
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
