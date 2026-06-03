#include "command_stream.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>

namespace vkfwd {
namespace {

struct TwoBytes {
    std::uint8_t first  = 0;
    std::uint8_t second = 0;
};

TEST_CASE("command stream reset can preserve a fixed request header") {
    constexpr StreamId kStreamId = 0x1234;
    CommandStream      stream(128);

    stream.reset(kStreamId);
    REQUIRE(stream.size() == sizeof(CommandStream::StreamHeader));
    auto header_view = stream.at<CommandStream::StreamHeader>(0);
    REQUIRE(!header_view.empty());
    CHECK(header_view.at(0).magic == CommandStream::StreamHeader {}.magic);
    CHECK(header_view.at(0).revision == CommandStream::StreamHeader {}.revision);
    CHECK(header_view.at(0).stream_id == kStreamId);

    stream.grow<std::uint32_t>() = 0xbeef;
    stream.reset();

    REQUIRE(stream.size() == sizeof(CommandStream::StreamHeader));
    header_view = stream.at<CommandStream::StreamHeader>(0);
    REQUIRE(!header_view.empty());
    CHECK(header_view.at(0).stream_id == kStreamId);
}

TEST_CASE("command stream closes chunks with explicit gap records") {
    CommandStream stream(128);

    const std::array<std::uint8_t, CommandStream::kMinimumChunkSize - sizeof(CommandStreamGapHeader)> first_bytes {};
    auto                                first = stream.grow(first_bytes.size());
    REQUIRE(first.set(0, first_bytes.size(), first_bytes.data()) == first_bytes.size());

    const std::array<std::uint8_t, 4> second_bytes {0x40, 0x50, 0x60, 0x70};
    std::size_t                       second_offset = 0;
    auto                              second        = stream.grow(second_bytes.size(), alignof(std::uint32_t), &second_offset);
    REQUIRE(second.set(0, second_bytes.size(), second_bytes.data()) == second_bytes.size());

    REQUIRE_FALSE(stream.is_contiguous());
    CHECK(second_offset == CommandStream::kMinimumChunkSize);

    CommandStream flattened = stream.flatten();
    REQUIRE(flattened.is_contiguous());
    REQUIRE(flattened.size() == stream.size());

    CommandStreamGapHeader gap {};
    const auto             gap_bytes = flattened.at(first_bytes.size(), sizeof(gap));
    REQUIRE(!gap_bytes.empty());
    std::memcpy(&gap, gap_bytes.address(0), sizeof(gap));
    CHECK(gap.magic == kCommandStreamGapMagic);
    CHECK(gap.size == CommandStream::kMinimumChunkSize - first_bytes.size());

    const auto second_view = flattened.at(second_offset, second_bytes.size());
    REQUIRE(!second_view.empty());
    for (std::size_t index = 0; index < second_bytes.size(); ++index) { CHECK(second_view.at(index) == second_bytes.at(index)); }
}

TEST_CASE("command stream keeps sized arrays contiguous") {
    CommandStream stream(128);

    auto prefix = stream.grow<std::uint8_t>(CommandStream::kMinimumChunkSize - sizeof(CommandStreamGapHeader));
    REQUIRE(prefix.size() == CommandStream::kMinimumChunkSize - sizeof(CommandStreamGapHeader));

    std::size_t second_offset = 0;
    auto        second        = stream.grow<std::uint32_t>(3, alignof(std::uint32_t), &second_offset);
    REQUIRE(second.size() == 3);
    CHECK(second_offset == CommandStream::kMinimumChunkSize);
}

TEST_CASE("typed stream access uses byte offsets") {
    CommandStream stream(8);

    const std::array<std::uint8_t, 3> bytes {0x10, 0x20, 0x30};
    auto                              storage = stream.grow(bytes.size());
    REQUIRE(storage.set(0, bytes.size(), bytes.data()) == bytes.size());

    CHECK(!stream.at<TwoBytes>(1).empty());
    CHECK(stream.at<TwoBytes>(1, sizeof(TwoBytes) - 1).empty());
}

} // namespace
} // namespace vkfwd
