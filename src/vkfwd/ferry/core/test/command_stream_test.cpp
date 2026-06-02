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

TEST_CASE("command stream closes chunks with explicit gap records") {
    CommandStream stream(128);

    const std::array<std::uint8_t, 120> first_bytes {};
    auto                                first = stream.grow(first_bytes.size());
    REQUIRE(first.set(0, first_bytes.size(), first_bytes.data()) == first_bytes.size());

    const std::array<std::uint8_t, 4> second_bytes {0x40, 0x50, 0x60, 0x70};
    std::size_t                       second_offset = 0;
    auto                              second        = stream.grow(second_bytes.size(), alignof(std::uint32_t), &second_offset);
    REQUIRE(second.set(0, second_bytes.size(), second_bytes.data()) == second_bytes.size());

    REQUIRE_FALSE(stream.is_contiguous());
    CHECK(second_offset == CommandStream::kBaseAlignment);

    CommandStream flattened = stream.flatten();
    REQUIRE(flattened.is_contiguous());
    REQUIRE(flattened.size() == stream.size());

    CommandStreamGapHeader gap {};
    const auto             gap_bytes = flattened.at(first_bytes.size(), sizeof(gap));
    REQUIRE(!gap_bytes.empty());
    std::memcpy(&gap, gap_bytes.address(0), sizeof(gap));
    CHECK(gap.magic == kCommandStreamGapMagic);
    CHECK(gap.size == CommandStream::kBaseAlignment - first_bytes.size());

    const auto second_view = flattened.at(second_offset, second_bytes.size());
    REQUIRE(!second_view.empty());
    for (std::size_t index = 0; index < second_bytes.size(); ++index) { CHECK(second_view.at(index) == second_bytes.at(index)); }
}

TEST_CASE("command stream keeps sized arrays contiguous") {
    CommandStream stream(128);

    auto prefix = stream.grow<std::uint8_t>(120);
    REQUIRE(prefix.size() == 120);

    std::size_t second_offset = 0;
    auto        second        = stream.grow<std::uint32_t>(3, alignof(std::uint32_t), &second_offset);
    REQUIRE(second.size() == 3);
    CHECK(second_offset == CommandStream::kBaseAlignment);
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
