#include "memory_map/vm_primitives.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::memory_map::vm::test {

TEST_CASE("vm::reserve returns a page-aligned base or null") {
    constexpr std::size_t kReservationSize = 4 * 1024 * 1024; // 4 MiB
    void * const          base             = reserve(kReservationSize);
    REQUIRE(base != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(base) % host_page_size() == 0);
    release(base, kReservationSize);
}

TEST_CASE("vm::commit makes a sub-range readable and writable") {
    constexpr std::size_t kReservationSize = 1 * 1024 * 1024;
    void * const          base             = reserve(kReservationSize);
    REQUIRE(base != nullptr);

    // Pick offset/size as multiples of the host page so the same test exercises
    // a real sub-range commit on 4 KiB (x86 Linux), 16 KiB (Apple Silicon),
    // and 64 KiB (Windows allocation granularity) hosts without rewriting.
    const std::size_t kCommitOffset = host_page_size();
    const std::size_t kCommitSize   = host_page_size() * 2;

    REQUIRE(commit(static_cast<std::uint8_t *>(base) + kCommitOffset, kCommitSize));

    auto * bytes = static_cast<std::uint8_t *>(base) + kCommitOffset;
    for (std::size_t i = 0; i < kCommitSize; ++i) { bytes[i] = static_cast<std::uint8_t>(i & 0xff); }
    for (std::size_t i = 0; i < kCommitSize; ++i) { CHECK(bytes[i] == static_cast<std::uint8_t>(i & 0xff)); }

    release(base, kReservationSize);
}

TEST_CASE("vm::release fully reclaims a reservation") {
    constexpr std::size_t kSize = 2 * 1024 * 1024;
    void *                first = reserve(kSize);
    REQUIRE(first != nullptr);
    release(first, kSize);

    void * second = reserve(kSize);
    REQUIRE(second != nullptr);
    release(second, kSize);
}

} // namespace vkfwd::memory_map::vm::test
