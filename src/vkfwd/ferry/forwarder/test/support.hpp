#pragma once

#include "command_stream.hpp"
#include "forwarder.hpp"
#include "transport_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string_view>

namespace vkfwd::forwarder::test {

using FlushHandler = CommandStream (*)(CommandStream & request_stream);

struct TransportState {
    FlushHandler handler   = nullptr;
    bool         processed = false;
};

inline TransportState & transport_state() {
    static TransportState state;
    return state;
}

class PackUnpackTransportSession final : public TransportSession {
public:
    CommandStream send_accumulated_api_calls(CommandStream & request_stream) override {
        auto & state    = transport_state();
        state.processed = true;
        REQUIRE(state.handler != nullptr);
        return state.handler(request_stream);
    }
};

inline std::shared_ptr<TransportSession> make_pack_unpack_transport_session() { return std::make_shared<PackUnpackTransportSession>(); }

inline void install_pack_unpack_transport(FlushHandler handler) {
    auto & state    = transport_state();
    state.handler   = handler;
    state.processed = false;
    Forwarder::set_transport_creator(make_pack_unpack_transport_session);
    Forwarder::instance().reset_request_stream();
}

inline Range first_command_range(const CommandStream & request_stream) {
    // Transport tests reconstruct chunk metadata from the stream header because
    // the forwarding boundary only transports stream bytes. The fixed request
    // header carries stream routing; closed chunk tails are explicit gap
    // records and ordinary alignment padding is skipped by offset math.
    std::size_t offset = sizeof(RequestStreamHeader);
    while (offset < request_stream.size()) {
        const auto gap_view = request_stream.at(offset, sizeof(CommandStreamGapHeader));
        if (!gap_view.empty()) {
            CommandStreamGapHeader gap {};
            std::memcpy(&gap, gap_view.address(0), sizeof(gap));
            if (gap.magic == kCommandStreamGapMagic) {
                REQUIRE(gap.size >= sizeof(CommandStreamGapHeader));
                offset += gap.size;
                continue;
            }
        }
        const std::size_t remainder = offset % CommandStream::kBaseAlignment;
        if (remainder != 0) {
            offset += CommandStream::kBaseAlignment - remainder;
            continue;
        }
        const auto header_view = request_stream.at(offset, sizeof(CommandChunkHeader));
        if (!header_view.empty()) {
            const auto * header      = reinterpret_cast<const CommandChunkHeader *>(&header_view.at(0));
            const auto   next_offset = offset + header->size;
            if (header->size >= sizeof(CommandChunkHeader) && next_offset >= offset && next_offset <= request_stream.size()) {
                return Range {.offset = offset, .size = header->size};
            }
        }
        break;
    }
    FAIL("request stream does not contain a command chunk");
    return {};
}

template<class Pointer>
std::size_t encoded_offset(Pointer pointer) {
    return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

inline SafeArrayView<std::uint8_t> command_view(CommandStream & stream, const Range & packet) { return stream.at(packet.offset, packet.size); }

template<class T>
constexpr std::size_t command_payload_offset() {
    constexpr std::size_t alignment = alignof(T);
    return (sizeof(CommandChunkHeader) + alignment - 1) & ~(alignment - 1);
}

template<class T>
std::size_t command_payload_blob_offset(const Range & packet) {
    return packet.offset + command_payload_offset<T>();
}

template<class Pointer>
bool points_into_view(SafeArrayView<std::uint8_t> & view, Pointer pointer) {
    auto * begin = view.address(0);
    if (!begin || !pointer) { return false; }
    const auto * target = reinterpret_cast<const std::uint8_t *>(pointer);
    return target >= begin && target < begin + view.size();
}

template<class Object, class Pointer>
std::size_t field_relative_target_offset(const CommandStream & stream, std::size_t object_offset, Pointer Object::* field) {
    const auto object_view = stream.at(object_offset, sizeof(Object));
    REQUIRE(!object_view.empty());
    const auto *         object  = reinterpret_cast<const Object *>(&object_view.at(0));
    const auto *         slot    = reinterpret_cast<const std::uint8_t *>(&(object->*field));
    const std::uintptr_t encoded = reinterpret_cast<std::uintptr_t>(object->*field);
    const auto *         begin   = reinterpret_cast<const std::uint8_t *>(object);
    return object_offset + static_cast<std::size_t>((slot + encoded) - begin);
}

template<class Object, class Pointer>
void check_field_relative_pointer(const CommandStream & stream, std::size_t object_offset, Pointer Object::* field, std::size_t target_offset) {
    const auto object_view = stream.at(object_offset, sizeof(Object));
    REQUIRE(!object_view.empty());
    const auto *      object      = reinterpret_cast<const Object *>(&object_view.at(0));
    const auto *      slot        = reinterpret_cast<const std::uint8_t *>(&(object->*field));
    const auto *      begin       = reinterpret_cast<const std::uint8_t *>(object);
    const std::size_t slot_offset = object_offset + static_cast<std::size_t>(slot - begin);
    CHECK(encoded_offset(object->*field) == target_offset - slot_offset);
}

template<class T>
const T & object_at(const CommandStream & stream, std::size_t offset) {
    const auto view = stream.at(offset, sizeof(T));
    REQUIRE(!view.empty());
    return *reinterpret_cast<const T *>(&view.at(0));
}

inline void check_relative_string(const CommandStream & stream, std::size_t base_offset, const char * encoded_value, std::string_view expected) {
    (void) stream;
    (void) base_offset;
    REQUIRE(encoded_value != nullptr);
    CHECK(std::string_view(encoded_value, expected.size()) == expected);
    CHECK(encoded_value[expected.size()] == '\0');
}

inline void check_relative_string_array(const CommandStream & stream, std::size_t base_offset, const char * const * encoded_values,
                                        std::initializer_list<std::string_view> expected) {
    (void) stream;
    (void) base_offset;
    if (expected.size() == 0) {
        CHECK(encoded_values == nullptr);
        return;
    }

    REQUIRE(encoded_values != nullptr);
    std::size_t index = 0;
    for (std::string_view value : expected) {
        const auto * string = encoded_values[index];
        REQUIRE(string != nullptr);
        CHECK(std::string_view(string, value.size()) == value);
        CHECK(string[value.size()] == '\0');
        ++index;
    }
}

inline void * VKAPI_PTR test_allocation(void *, std::size_t, std::size_t, VkSystemAllocationScope) { return nullptr; }

inline void * VKAPI_PTR test_reallocation(void *, void *, std::size_t, std::size_t, VkSystemAllocationScope) { return nullptr; }

inline void VKAPI_PTR test_free(void *, void *) {}

inline void VKAPI_PTR test_internal_allocation(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {}

inline void VKAPI_PTR test_internal_free(void *, std::size_t, VkInternalAllocationType, VkSystemAllocationScope) {}

inline VkAllocationCallbacks test_allocator(void * user_data) {
    return VkAllocationCallbacks {
        .pUserData             = user_data,
        .pfnAllocation         = test_allocation,
        .pfnReallocation       = test_reallocation,
        .pfnFree               = test_free,
        .pfnInternalAllocation = test_internal_allocation,
        .pfnInternalFree       = test_internal_free,
    };
}

template<class Handle>
Handle test_handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

} // namespace vkfwd::forwarder::test
