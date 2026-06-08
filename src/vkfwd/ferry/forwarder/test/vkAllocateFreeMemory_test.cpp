#include "support.hpp"

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using AllocateCommand = ::vkfwd::generated::commands::vkAllocateMemory::Command;

constexpr VkDeviceSize kAllocationSize = 64 * 1024;

struct Scenario {
    VkDevice             device          = test_handle<VkDevice>(0x501);
    VkDeviceMemory       receiver_memory = test_handle<VkDeviceMemory>(0x601);
    VkMemoryAllocateInfo allocate_info {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = kAllocationSize,
        .memoryTypeIndex = 2,
    };
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

CommandStream handle_allocate_flush(CommandStream & request_stream) {
    auto &       expected = scenario();
    const Range  packet   = first_command_range(request_stream);
    auto         bytes    = command_view(request_stream, packet);
    const auto * actual   = static_cast<const AllocateCommand::Parameters *>(nullptr);
    REQUIRE(AllocateCommand::unpack_parameters(bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->device == expected.device);
    REQUIRE(actual->pAllocateInfo != nullptr);
    CHECK(actual->pAllocateInfo->allocationSize == expected.allocate_info.allocationSize);

    CommandStream             response_stream;
    AllocateCommand::Response response {.return_value = VK_SUCCESS, .pMemory = &expected.receiver_memory};
    REQUIRE(AllocateCommand::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

} // namespace

TEST_CASE("vkAllocateMemory and vkFreeMemory update memory map allocation records") {
    auto & manager  = ::vkfwd::MemoryMapForwarder::instance();
    auto & expected = scenario();
    manager.forget_allocation(expected.receiver_memory);
    install_pack_unpack_transport(handle_allocate_flush);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkAllocateMemory_entry(expected.device, &expected.allocate_info, nullptr, &memory);

    REQUIRE(result == VK_SUCCESS);
    CHECK(memory == expected.receiver_memory);
    VkDeviceSize recorded_size = manager.test_get_allocation_size(memory);
    CHECK(recorded_size == expected.allocate_info.allocationSize);

    vkfwd::forwarder::generated::vkFreeMemory_entry(expected.device, memory, nullptr);

    CHECK(manager.test_get_allocation_size(memory) == 0);
}

} // namespace vkfwd::forwarder::test
