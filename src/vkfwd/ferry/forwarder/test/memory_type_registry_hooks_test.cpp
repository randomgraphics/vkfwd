#include "support.hpp"

#include "generated/command/vkCreateDevice.hpp"
#include "generated/command/vkDestroyDevice.hpp"
#include "generated/command/vkGetPhysicalDeviceMemoryProperties.hpp"
#include "generated/command/vkGetPhysicalDeviceProperties.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using ::vkfwd::memory_map::MemoryTypeRegistry;

// One static scenario per test — the test transport handlers are
// install_pack_unpack_transport-style C function pointers (no captures), so
// they must reach test data through file-scope state, the same pattern
// vkCreateDevice_test.cpp uses.
struct RegistryHookScenario {
    VkPhysicalDevice physical_device       = test_handle<VkPhysicalDevice>(0x310);
    VkDevice         created_device        = test_handle<VkDevice>(0x410);
    VkDeviceSize     atom_size_to_publish  = 128;
    std::size_t      map_alignment_to_pub  = 4096;
    VkResult         create_device_result  = VK_SUCCESS;
};

RegistryHookScenario & scenario() {
    static RegistryHookScenario s;
    return s;
}

VkPhysicalDeviceMemoryProperties make_props() {
    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 1;
    props.memoryTypes[0]  = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    return props;
}

CommandStream handle_create_device(CommandStream & request_stream) {
    using Command          = ::vkfwd::generated::commands::vkCreateDevice::Command;
    const auto & expected  = scenario();
    const Range  packet    = first_command_range(request_stream);
    auto         bytes     = command_view(request_stream, packet);
    const Command::Parameters * params = nullptr;
    REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);
    REQUIRE(params != nullptr);
    REQUIRE(params->pDevice != nullptr);

    *params->pDevice = expected.created_device;

    CommandStream     response_stream;
    Command::Response r {.return_value = expected.create_device_result,
                          .pDevice      = params->pDevice};
    REQUIRE(Command::pack_response(response_stream, r) == VK_SUCCESS);
    return response_stream;
}

CommandStream handle_get_memory_properties(CommandStream & request_stream) {
    using Command          = ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command;
    const Range packet     = first_command_range(request_stream);
    auto        bytes      = command_view(request_stream, packet);
    const Command::Parameters * params = nullptr;
    REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);
    REQUIRE(params != nullptr);
    REQUIRE(params->pMemoryProperties != nullptr);

    *params->pMemoryProperties = make_props();

    CommandStream     response_stream;
    Command::Response r {.pMemoryProperties = params->pMemoryProperties};
    REQUIRE(Command::pack_response(response_stream, r) == VK_SUCCESS);
    return response_stream;
}

CommandStream handle_get_properties(CommandStream & request_stream) {
    using Command          = ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command;
    const auto & expected  = scenario();
    const Range  packet    = first_command_range(request_stream);
    auto         bytes     = command_view(request_stream, packet);
    const Command::Parameters * params = nullptr;
    REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);
    REQUIRE(params != nullptr);
    REQUIRE(params->pProperties != nullptr);

    VkPhysicalDeviceProperties out {};
    out.limits.nonCoherentAtomSize   = expected.atom_size_to_publish;
    out.limits.minMemoryMapAlignment = expected.map_alignment_to_pub;
    *params->pProperties             = out;

    CommandStream     response_stream;
    Command::Response r {.pProperties = params->pProperties};
    REQUIRE(Command::pack_response(response_stream, r) == VK_SUCCESS);
    return response_stream;
}

CommandStream handle_destroy_device(CommandStream & /*request_stream*/) {
    return CommandStream {};
}

} // namespace

TEST_CASE("vkCreateDevice forwarder hook records device->physicalDevice mapping") {
    auto & registry = MemoryTypeRegistry::instance();
    auto & s        = scenario();
    s               = RegistryHookScenario {};
    s.physical_device = test_handle<VkPhysicalDevice>(0x310);
    s.created_device  = test_handle<VkDevice>(0x410);

    registry.forget_device(s.created_device);

    install_pack_unpack_transport(handle_create_device);

    VkDeviceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    VkDevice       device = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkCreateDevice_entry(
        s.physical_device, &create_info, nullptr, &device);
    REQUIRE(result == VK_SUCCESS);
    CHECK(device == s.created_device);

    // resolve() needs all four preconditions; seed the other three to confirm
    // the device->physical association the hook just stored is in the cache.
    registry.record_memory_properties(s.physical_device, make_props());
    registry.record_non_coherent_atom_size(s.physical_device, 64);
    registry.record_min_memory_map_alignment(s.physical_device, 4096);
    const auto resolved = registry.resolve(s.created_device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    registry.forget_device(s.created_device);
}

TEST_CASE("vkGetPhysicalDeviceMemoryProperties forwarder hook records memory properties") {
    auto & registry = MemoryTypeRegistry::instance();
    auto & s        = scenario();
    s               = RegistryHookScenario {};
    s.physical_device = test_handle<VkPhysicalDevice>(0x311);
    s.created_device  = test_handle<VkDevice>(0x411);

    registry.forget_device(s.created_device);
    registry.record_device(s.created_device, s.physical_device);
    registry.record_non_coherent_atom_size(s.physical_device, 32);
    registry.record_min_memory_map_alignment(s.physical_device, 4096);

    install_pack_unpack_transport(handle_get_memory_properties);

    VkPhysicalDeviceMemoryProperties props {};
    vkfwd::forwarder::generated::vkGetPhysicalDeviceMemoryProperties_entry(
        s.physical_device, &props);

    const auto resolved = registry.resolve(s.created_device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    registry.forget_device(s.created_device);
}

TEST_CASE("vkGetPhysicalDeviceProperties forwarder hook records nonCoherentAtomSize and minMemoryMapAlignment") {
    auto & registry = MemoryTypeRegistry::instance();
    auto & s        = scenario();
    s               = RegistryHookScenario {};
    s.physical_device       = test_handle<VkPhysicalDevice>(0x312);
    s.created_device        = test_handle<VkDevice>(0x412);
    s.atom_size_to_publish  = 128;
    s.map_alignment_to_pub  = 4096;

    registry.forget_device(s.created_device);
    registry.record_device(s.created_device, s.physical_device);
    registry.record_memory_properties(s.physical_device, make_props());

    install_pack_unpack_transport(handle_get_properties);

    VkPhysicalDeviceProperties props {};
    vkfwd::forwarder::generated::vkGetPhysicalDeviceProperties_entry(
        s.physical_device, &props);

    const auto resolved = registry.resolve(s.created_device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->non_coherent_atom_size == s.atom_size_to_publish);
    CHECK(resolved->min_memory_map_alignment == s.map_alignment_to_pub);

    registry.forget_device(s.created_device);
}

TEST_CASE("vkDestroyDevice forwarder hook removes the device entry") {
    auto & registry = MemoryTypeRegistry::instance();
    auto & s        = scenario();
    s               = RegistryHookScenario {};
    s.physical_device = test_handle<VkPhysicalDevice>(0x313);
    s.created_device  = test_handle<VkDevice>(0x413);

    registry.record_device(s.created_device, s.physical_device);
    registry.record_memory_properties(s.physical_device, make_props());
    registry.record_non_coherent_atom_size(s.physical_device, 64);
    registry.record_min_memory_map_alignment(s.physical_device, 4096);
    REQUIRE(registry.resolve(s.created_device, 0).has_value());

    install_pack_unpack_transport(handle_destroy_device);

    vkfwd::forwarder::generated::vkDestroyDevice_entry(s.created_device, nullptr);

    CHECK_FALSE(registry.resolve(s.created_device, 0).has_value());
}

} // namespace vkfwd::forwarder::test
