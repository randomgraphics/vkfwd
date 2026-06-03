#include "support.hpp"

#include "generated/command/vkCreateInstance.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "generated/structure/core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::forwarder::test {
namespace {

using Command = ::vkfwd::generated::commands::vkCreateInstance::Command;

struct Scenario {
    int                         allocator_user_data = 0x31;
    VkAllocationCallbacks       allocator;
    VkApplicationInfo           application_info;
    std::array<const char *, 2> layers;
    std::array<const char *, 2> extensions;
    VkInstanceCreateInfo        create_info;
    VkInstance *                output_instance = nullptr;
    VkInstance                  response_instance;
    VkResult                    response_result = VK_INCOMPLETE;
};

Scenario make_scenario() {
    Scenario scenario;
    scenario.allocator        = test_allocator(&scenario.allocator_user_data);
    scenario.application_info = VkApplicationInfo {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext              = nullptr,
        .pApplicationName   = "vkfwd-create-instance-app",
        .applicationVersion = 7,
        .pEngineName        = "vkfwd-create-instance-engine",
        .engineVersion      = 11,
        .apiVersion         = VK_MAKE_API_VERSION(0, 1, 2, 3),
    };
    scenario.layers      = {"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"};
    scenario.extensions  = {"VK_EXT_debug_utils", "VK_KHR_surface"};
    scenario.create_info = VkInstanceCreateInfo {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkInstanceCreateFlags {0x4},
        .pApplicationInfo        = &scenario.application_info,
        .enabledLayerCount       = static_cast<std::uint32_t>(scenario.layers.size()),
        .ppEnabledLayerNames     = scenario.layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(scenario.extensions.size()),
        .ppEnabledExtensionNames = scenario.extensions.data(),
    };
    scenario.response_instance = test_handle<VkInstance>(0x101);
    return scenario;
}

Scenario & scenario() {
    static Scenario value = make_scenario();
    return value;
}

CommandStream handle_flush(CommandStream & request_stream) {
    auto &     expected = scenario();
    const auto packet   = first_command_range(request_stream);

    const auto   parameters_offset  = command_payload_blob_offset<Command::Parameters>(packet);
    const auto & raw_parameters     = object_at<Command::Parameters>(request_stream, parameters_offset);
    const auto   create_info_offset = field_relative_target_offset(request_stream, parameters_offset, &Command::Parameters::pCreateInfo);
    const auto   allocator_offset   = field_relative_target_offset(request_stream, parameters_offset, &Command::Parameters::pAllocator);
    check_field_relative_pointer(request_stream, parameters_offset, &Command::Parameters::pCreateInfo, create_info_offset);
    check_field_relative_pointer(request_stream, parameters_offset, &Command::Parameters::pAllocator, allocator_offset);
    CHECK(raw_parameters.pAllocator == nullptr);
    const auto instance_output_offset = field_relative_target_offset(request_stream, parameters_offset, &Command::Parameters::pInstance);
    check_field_relative_pointer(request_stream, parameters_offset, &Command::Parameters::pInstance, instance_output_offset);

    const auto & raw_create_info         = object_at<VkInstanceCreateInfo>(request_stream, create_info_offset);
    const auto   application_info_offset = field_relative_target_offset(request_stream, create_info_offset, &VkInstanceCreateInfo::pApplicationInfo);
    check_field_relative_pointer(request_stream, create_info_offset, &VkInstanceCreateInfo::pApplicationInfo, application_info_offset);

    auto                        command_bytes = command_view(request_stream, packet);
    const Command::Parameters * actual        = nullptr;
    REQUIRE(Command::unpack_parameters(command_bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    REQUIRE(actual->pInstance != nullptr);
    CHECK(actual->pInstance != expected.output_instance);
    CHECK(points_into_view(command_bytes, actual->pInstance));

    REQUIRE(actual->pCreateInfo != nullptr);
    const VkInstanceCreateInfo * packed_create_info = actual->pCreateInfo;
    REQUIRE(packed_create_info != nullptr);
    CHECK(packed_create_info->pNext == nullptr);
    CHECK(packed_create_info->flags == expected.create_info.flags);
    CHECK(packed_create_info->enabledLayerCount == expected.create_info.enabledLayerCount);
    CHECK(packed_create_info->enabledExtensionCount == expected.create_info.enabledExtensionCount);

    REQUIRE(packed_create_info->pApplicationInfo != nullptr);
    const VkApplicationInfo * packed_application_info = packed_create_info->pApplicationInfo;
    REQUIRE(packed_application_info != nullptr);
    CHECK(packed_application_info->pNext == nullptr);
    CHECK(packed_application_info->applicationVersion == expected.application_info.applicationVersion);
    CHECK(packed_application_info->engineVersion == expected.application_info.engineVersion);
    CHECK(packed_application_info->apiVersion == expected.application_info.apiVersion);
    check_relative_string(request_stream, 0, packed_application_info->pApplicationName, expected.application_info.pApplicationName);
    check_relative_string(request_stream, 0, packed_application_info->pEngineName, expected.application_info.pEngineName);
    check_relative_string_array(request_stream, 0, packed_create_info->ppEnabledLayerNames, {"VK_LAYER_VKFWD_alpha", "VK_LAYER_VKFWD_beta"});
    check_relative_string_array(request_stream, 0, packed_create_info->ppEnabledExtensionNames, {"VK_EXT_debug_utils", "VK_KHR_surface"});

    CHECK(actual->pAllocator == nullptr);

    CommandStream     response_stream;
    Command::Response response {.return_value = expected.response_result, .pInstance = &expected.response_instance};
    REQUIRE(Command::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

} // namespace

TEST_CASE("vkCreateInstance forwarder entry point round trips packed parameters and response") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_flush);

    VkInstance instance      = VK_NULL_HANDLE;
    expected.output_instance = &instance;
    const VkResult result    = vkfwd::forwarder::generated::vkCreateInstance_entry(&expected.create_info, &expected.allocator, &instance);

    CHECK(transport_state().processed);
    CHECK(result == expected.response_result);
    CHECK(instance == expected.response_instance);
}

} // namespace vkfwd::forwarder::test
