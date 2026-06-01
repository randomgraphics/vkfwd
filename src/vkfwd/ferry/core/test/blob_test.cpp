#include "blob.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd {
namespace {

TEST_CASE("blob flatten preserves logical bytes in one allocation") {
    Blob blob(4);

    const std::array<std::uint8_t, 3> first_bytes {0x10, 0x20, 0x30};
    auto                             first = blob.grow(first_bytes.size());
    REQUIRE(first.set(0, first_bytes.size(), first_bytes.data()) == first_bytes.size());

    const std::array<std::uint8_t, 4> second_bytes {0x40, 0x50, 0x60, 0x70};
    auto                             second = blob.grow(second_bytes.size(), alignof(std::uint32_t));
    REQUIRE(second.set(0, second_bytes.size(), second_bytes.data()) == second_bytes.size());

    REQUIRE_FALSE(blob.is_contiguous());

    Blob flattened = blob.flatten();
    REQUIRE(flattened.is_contiguous());
    REQUIRE(flattened.size() == blob.size());

    // Flattening is a transport convenience only; logical offsets and payload
    // bytes must remain stable even when the original arena had multiple chunks.
    const auto bytes = flattened.at(0, flattened.size());
    REQUIRE(bytes.size() == first_bytes.size() + second_bytes.size());

    for (std::size_t index = 0; index < first_bytes.size(); ++index) { CHECK(bytes.at(index) == first_bytes.at(index)); }
    for (std::size_t index = 0; index < second_bytes.size(); ++index) {
        CHECK(bytes.at(first_bytes.size() + index) == second_bytes.at(index));
    }
}

} // namespace
} // namespace vkfwd
