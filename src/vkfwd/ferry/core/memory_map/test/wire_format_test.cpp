#include "memory_map/wire_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace vkfwd::memory_map::wire::test {

TEST_CASE("MemoryMapRequest is POD and round-trips via memcpy") {
    MemoryMapRequest req {
        .manager_revision = kMemoryMapManagerRevision,
        .device           = reinterpret_cast<VkDevice>(0x1234),
        .memory           = reinterpret_cast<VkDeviceMemory>(0x5678),
        .offset           = 0x1000,
        .size             = VK_WHOLE_SIZE,
        .flags            = 0,
    };

    std::uint8_t buffer[sizeof(req)] {};
    std::memcpy(buffer, &req, sizeof(req));

    MemoryMapRequest out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == req.manager_revision);
    CHECK(out.device == req.device);
    CHECK(out.memory == req.memory);
    CHECK(out.offset == req.offset);
    CHECK(out.size == req.size);
    CHECK(out.flags == req.flags);
}

TEST_CASE("MemoryMapResponse round-trips via memcpy") {
    MemoryMapResponse resp {
        .manager_revision = kMemoryMapManagerRevision,
        .return_value     = VK_SUCCESS,
        .effective_size   = 0x4000,
    };

    std::uint8_t buffer[sizeof(resp)] {};
    std::memcpy(buffer, &resp, sizeof(resp));

    MemoryMapResponse out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == resp.manager_revision);
    CHECK(out.return_value == resp.return_value);
    CHECK(out.effective_size == resp.effective_size);
}

TEST_CASE("MemoryUnmapRequestHeader round-trips via memcpy") {
    MemoryUnmapRequestHeader header {
        .manager_revision = kMemoryMapManagerRevision,
        .range_count      = 0,
        .device           = reinterpret_cast<VkDevice>(0x1234),
        .memory           = reinterpret_cast<VkDeviceMemory>(0x5678),
        .mapped_offset    = 0x2000,
        .mapped_size      = 0x4000,
        .reserved         = 0,
        .pad0             = 0,
    };

    std::uint8_t buffer[sizeof(header)] {};
    std::memcpy(buffer, &header, sizeof(header));

    MemoryUnmapRequestHeader out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == header.manager_revision);
    CHECK(out.mapped_offset == header.mapped_offset);
    CHECK(out.range_count == header.range_count);
}

} // namespace vkfwd::memory_map::wire::test
