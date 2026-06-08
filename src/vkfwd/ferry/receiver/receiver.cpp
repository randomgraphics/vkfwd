#include "receiver.hpp"

#include "command_id_range.hpp"
#include "custom_command.hpp"
#include "generated/endpoints.hpp"
#include "logging.hpp"
#include "manual_dispatch.hpp"

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
        CommandStream response_stream;

        if (request_stream.size() < sizeof(RequestStreamHeader)) {
            VKFWD_LOG_ERROR("vkfwd receiver: request stream is missing stream header, size={}", request_stream.size());
            return response_stream;
        }
        const auto header_view = request_stream.at<RequestStreamHeader>(0);
        const auto header      = header_view.address();
        if (!header || header->magic != RequestStreamHeader {}.magic || header->revision != RequestStreamHeader {}.revision) {
            VKFWD_LOG_ERROR("vkfwd receiver: request stream has invalid header");
            return response_stream;
        }

        std::size_t offset = sizeof(RequestStreamHeader);
        while (offset < request_stream.size()) {
            while (offset < request_stream.size()) {
                CommandStreamGapHeader gap {};
                if (read_gap_header(request_stream, offset, gap)) {
                    if (gap.size < sizeof(CommandStreamGapHeader)) {
                        VKFWD_LOG_ERROR("vkfwd receiver: invalid stream gap size, offset={}, size={}", offset, gap.size);
                        return response_stream;
                    }
                    std::size_t next_offset = 0;
                    if (!checked_add(offset, gap.size, next_offset) || next_offset > request_stream.size()) {
                        VKFWD_LOG_ERROR("vkfwd receiver: stream gap exceeds request stream, offset={}, size={}, request_size={}", offset, gap.size,
                                        request_stream.size());
                        return response_stream;
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
                return response_stream;
            }
            if (header->size < sizeof(CommandChunkHeader)) {
                VKFWD_LOG_ERROR("vkfwd receiver: invalid command chunk size, offset={}, command_id={}, size={}", offset, header->command_id, header->size);
                return response_stream;
            }

            std::size_t next_offset = 0;
            if (!checked_add(offset, header->size, next_offset) || next_offset > request_stream.size()) {
                VKFWD_LOG_ERROR("vkfwd receiver: command chunk exceeds request stream, offset={}, command_id={}, size={}, request_size={}", offset,
                                header->command_id, header->size, request_stream.size());
                return response_stream;
            }

            const std::uint32_t raw_command_id = header->command_id;
            const Range         request_range {
                .offset = offset,
                .size   = header->size,
            };
            // The 32-bit command_id space is split at kReservedCommandIdBase:
            // generated Vulkan ids live below it, manual vkfwd ids above.
            // Branching here keeps the generated dispatcher unaware of the
            // manual protocol and lets the manual handler reject unknown ids
            // loudly instead of casting them into a generated-id table miss.
            bool ok = false;
            if (raw_command_id >= ::vkfwd::kReservedCommandIdBase) {
                ok = ::vkfwd::receiver::dispatch_manual_command(static_cast<::vkfwd::manual::CommandId>(raw_command_id), request_stream, request_range,
                                                                response_stream, replay_context_);
            } else {
                const auto command_id = static_cast<generated::CommandId>(raw_command_id);
                ok                    = receiver::generated::call_api_endpoint(command_id, request_stream, request_range, response_stream, replay_context_);
            }
            if (!ok) {
                VKFWD_LOG_ERROR("vkfwd receiver: failed to dispatch command_id={}", raw_command_id);
                return response_stream;
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
