#include "replay_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace vkfwd::receiver::test {
namespace {
template<class Handle>
Handle test_handle(std::uintptr_t v) {
    return reinterpret_cast<Handle>(v);
}
} // namespace

TEST_CASE("ReplayContext starts with empty handle maps") {
    ReplayContext context;
    CHECK(context.source_to_receiver_device.empty());
    CHECK(context.source_to_receiver_memory.empty());
}

TEST_CASE("ReplayContext handle maps round-trip insert and erase") {
    ReplayContext        context;
    const VkDevice       source_device   = test_handle<VkDevice>(0x1001);
    const VkDevice       receiver_device = test_handle<VkDevice>(0x1001); // Phase 1 identity invariant
    const VkDeviceMemory source_memory   = test_handle<VkDeviceMemory>(0x2001);
    const VkDeviceMemory receiver_memory = test_handle<VkDeviceMemory>(0x2001);

    context.source_to_receiver_device[source_device] = receiver_device;
    context.source_to_receiver_memory[source_memory] = receiver_memory;

    REQUIRE(context.source_to_receiver_device.count(source_device) == 1);
    REQUIRE(context.source_to_receiver_memory.count(source_memory) == 1);
    CHECK(context.source_to_receiver_device.at(source_device) == receiver_device);
    CHECK(context.source_to_receiver_memory.at(source_memory) == receiver_memory);

    context.source_to_receiver_device.erase(source_device);
    context.source_to_receiver_memory.erase(source_memory);
    CHECK(context.source_to_receiver_device.empty());
    CHECK(context.source_to_receiver_memory.empty());
}

} // namespace vkfwd::receiver::test
