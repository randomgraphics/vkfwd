#include "support.hpp"

#include "generated/forwarder_entrypoints.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

// Once FORWARDER_MEMORY_MAP_MANAGED_COMMANDS is enabled for these two APIs,
// the generated entry points delegate to MemoryMapForwarder rather than
// emitting a generated Vulkan command chunk. With no record for the handle
// in the manager (the tests do not allocate first), the manager rejects the
// call at the surface and the wire is never touched.

struct Scenario {
    VkDevice         device = test_handle<VkDevice>(0x701);
    VkDeviceMemory   memory = test_handle<VkDeviceMemory>(0x801);
    VkDeviceSize     offset = 256;
    VkDeviceSize     size   = 4096;
    VkMemoryMapFlags flags  = VkMemoryMapFlags {0};
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

CommandStream handle_must_not_be_called(CommandStream & /*request_stream*/) {
    FAIL("vkMapMemory_entry should not flush the wire when the handle is unrecorded");
    return {};
}

} // namespace

TEST_CASE("vkMapMemory_entry delegates to MemoryMapForwarder; unrecorded handle returns VK_ERROR_FEATURE_NOT_PRESENT without touching the wire") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_must_not_be_called);

    void *         mapped = reinterpret_cast<void *>(0xdead);
    const VkResult result =
        vkfwd::forwarder::generated::vkMapMemory_entry(expected.device, expected.memory, expected.offset, expected.size, expected.flags, &mapped);

    CHECK(result == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(mapped == nullptr);
    CHECK_FALSE(transport_state().processed);
}

TEST_CASE("vkUnmapMemory_entry delegates to MemoryMapForwarder; unrecorded handle is a manager-side no-op without touching the wire") {
    auto & expected = scenario();
    Forwarder::instance().reset_request_stream();
    install_pack_unpack_transport(handle_must_not_be_called);

    vkfwd::forwarder::generated::vkUnmapMemory_entry(expected.device, expected.memory);

    // No generated chunk appended, no wire flush.
    CHECK(Forwarder::instance().request_stream().size() == sizeof(::vkfwd::RequestStreamHeader));
    CHECK_FALSE(transport_state().processed);
}

} // namespace vkfwd::forwarder::test
