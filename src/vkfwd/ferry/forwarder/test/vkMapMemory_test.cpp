#include "support.hpp"

#include "generated/command/vkMapMemory.hpp"
#include "generated/command/vkUnmapMemory.hpp"
#include "generated/forwarder_entrypoints.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using Command      = ::vkfwd::generated::commands::vkMapMemory::Command;
using UnmapCommand = ::vkfwd::generated::commands::vkUnmapMemory::Command;

struct Scenario {
    VkDevice         device        = test_handle<VkDevice>(0x701);
    VkDeviceMemory   memory        = test_handle<VkDeviceMemory>(0x801);
    VkDeviceSize     offset        = 256;
    VkDeviceSize     size          = 4096;
    void *           receiver_data = reinterpret_cast<void *>(0x901);
    VkMemoryMapFlags flags         = VkMemoryMapFlags {0};
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

CommandStream handle_map_flush(CommandStream & request_stream) {
    auto &      expected = scenario();
    const Range packet   = first_command_range(request_stream);
    auto        bytes    = command_view(request_stream, packet);

    const Command::Parameters * actual = nullptr;
    REQUIRE(Command::unpack_parameters(bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->device == expected.device);
    CHECK(actual->memory == expected.memory);
    CHECK(actual->offset == expected.offset);
    CHECK(actual->size == expected.size);
    CHECK(actual->flags == expected.flags);

    CommandStream     response_stream;
    Command::Response response {.return_value = VK_SUCCESS, .ppData = &expected.receiver_data};
    REQUIRE(Command::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

} // namespace

TEST_CASE("vkMapMemory forwarder uses generated flush path before memory map hook") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_map_flush);

    void *         mapped = reinterpret_cast<void *>(0xdead);
    const VkResult result =
        vkfwd::forwarder::generated::vkMapMemory_entry(expected.device, expected.memory, expected.offset, expected.size, expected.flags, &mapped);

    CHECK(transport_state().processed);
    CHECK(result == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(mapped == nullptr);
}

TEST_CASE("vkUnmapMemory forwarder appends generated command before memory map hook") {
    auto & expected = scenario();
    Forwarder::instance().reset_request_stream();

    vkfwd::forwarder::generated::vkUnmapMemory_entry(expected.device, expected.memory);

    auto &      request_stream = Forwarder::instance().request_stream();
    const Range packet         = first_command_range(request_stream);
    auto        bytes          = command_view(request_stream, packet);

    const UnmapCommand::Parameters * actual = nullptr;
    REQUIRE(UnmapCommand::unpack_parameters(bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->device == expected.device);
    CHECK(actual->memory == expected.memory);
}

} // namespace vkfwd::forwarder::test
