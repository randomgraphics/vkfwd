#include "support.hpp"

#include "generated/command/vkCreateDevice.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "generated/structure/core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

namespace vkfwd::forwarder::test {
namespace {

using Command = ::vkfwd::generated::commands::vkCreateDevice::Command;

struct Scenario {
    int                         allocator_user_data = 0x42;
    VkAllocationCallbacks       allocator;
    VkPhysicalDevice            physical_device = test_handle<VkPhysicalDevice>(0x202);
    std::array<float, 2>        queue_priorities;
    VkDeviceQueueCreateInfo     queue_create_info;
    std::array<const char *, 1> layers;
    std::array<const char *, 2> extensions;
    VkPhysicalDeviceFeatures    enabled_features;
    VkDeviceCreateInfo          create_info;
    VkDevice *                  output_device = nullptr;
    VkDevice                    response_device;
    VkResult                    response_result = VK_NOT_READY;
};

Scenario make_scenario() {
    Scenario scenario;
    scenario.allocator         = test_allocator(&scenario.allocator_user_data);
    scenario.queue_priorities  = {0.25f, 0.75f};
    scenario.queue_create_info = VkDeviceQueueCreateInfo {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VkDeviceQueueCreateFlags {0x2},
        .queueFamilyIndex = 3,
        .queueCount       = static_cast<std::uint32_t>(scenario.queue_priorities.size()),
        .pQueuePriorities = scenario.queue_priorities.data(),
    };
    scenario.layers                              = {"VK_LAYER_VKFWD_device"};
    scenario.extensions                          = {"VK_KHR_swapchain", "VK_EXT_private_data"};
    scenario.enabled_features                    = VkPhysicalDeviceFeatures {};
    scenario.enabled_features.robustBufferAccess = VK_TRUE;
    scenario.enabled_features.samplerAnisotropy  = VK_TRUE;
    scenario.create_info                         = VkDeviceCreateInfo {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = nullptr,
        .flags                   = VkDeviceCreateFlags {0x8},
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &scenario.queue_create_info,
        .enabledLayerCount       = static_cast<std::uint32_t>(scenario.layers.size()),
        .ppEnabledLayerNames     = scenario.layers.data(),
        .enabledExtensionCount   = static_cast<std::uint32_t>(scenario.extensions.size()),
        .ppEnabledExtensionNames = scenario.extensions.data(),
        .pEnabledFeatures        = &scenario.enabled_features,
    };
    scenario.response_device = test_handle<VkDevice>(0x303);
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
    CHECK(raw_parameters.physicalDevice == expected.physical_device);
    const auto device_output_offset = field_relative_target_offset(request_stream, parameters_offset, &Command::Parameters::pDevice);
    check_field_relative_pointer(request_stream, parameters_offset, &Command::Parameters::pDevice, device_output_offset);

    const auto queue_info_offset = field_relative_target_offset(request_stream, create_info_offset, &VkDeviceCreateInfo::pQueueCreateInfos);
    check_field_relative_pointer(request_stream, create_info_offset, &VkDeviceCreateInfo::pQueueCreateInfos, queue_info_offset);

    auto                        command_bytes = command_view(request_stream, packet);
    const Command::Parameters * actual        = nullptr;
    REQUIRE(Command::unpack_parameters(command_bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->physicalDevice == expected.physical_device);
    REQUIRE(actual->pDevice != nullptr);
    CHECK(actual->pDevice != expected.output_device);
    CHECK(points_into_view(command_bytes, actual->pDevice));

    REQUIRE(actual->pCreateInfo != nullptr);
    const VkDeviceCreateInfo * packed_create_info = actual->pCreateInfo;
    REQUIRE(packed_create_info != nullptr);
    CHECK(packed_create_info->pNext == nullptr);
    CHECK(packed_create_info->flags == expected.create_info.flags);
    CHECK(packed_create_info->queueCreateInfoCount == expected.create_info.queueCreateInfoCount);
    CHECK(packed_create_info->enabledLayerCount == expected.create_info.enabledLayerCount);
    CHECK(packed_create_info->enabledExtensionCount == expected.create_info.enabledExtensionCount);

    REQUIRE(packed_create_info->pQueueCreateInfos != nullptr);
    const VkDeviceQueueCreateInfo * packed_queue_info = packed_create_info->pQueueCreateInfos;
    REQUIRE(packed_queue_info != nullptr);
    CHECK(packed_queue_info->pNext == nullptr);
    CHECK(packed_queue_info->flags == expected.queue_create_info.flags);
    CHECK(packed_queue_info->queueFamilyIndex == expected.queue_create_info.queueFamilyIndex);
    CHECK(packed_queue_info->queueCount == expected.queue_create_info.queueCount);

    REQUIRE(packed_queue_info->pQueuePriorities != nullptr);
    const auto * priorities = packed_queue_info->pQueuePriorities;
    CHECK(priorities[0] == expected.queue_priorities[0]);
    CHECK(priorities[1] == expected.queue_priorities[1]);

    check_relative_string_array(request_stream, 0, packed_create_info->ppEnabledLayerNames, {"VK_LAYER_VKFWD_device"});
    check_relative_string_array(request_stream, 0, packed_create_info->ppEnabledExtensionNames, {"VK_KHR_swapchain", "VK_EXT_private_data"});

    REQUIRE(packed_create_info->pEnabledFeatures != nullptr);
    const auto & packed_features = *packed_create_info->pEnabledFeatures;
    CHECK(packed_features.robustBufferAccess == VK_TRUE);
    CHECK(packed_features.samplerAnisotropy == VK_TRUE);

    CHECK(actual->pAllocator == nullptr);

    CommandStream     response_stream;
    Command::Response response {.return_value = expected.response_result, .pDevice = &expected.response_device};
    REQUIRE(Command::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

} // namespace

TEST_CASE("vkCreateDevice forwarder entry point round trips packed parameters and response") {
    auto & expected = scenario();
    install_pack_unpack_transport(handle_flush);

    VkDevice device        = VK_NULL_HANDLE;
    expected.output_device = &device;
    const VkResult result  = vkfwd::forwarder::generated::vkCreateDevice_entry(expected.physical_device, &expected.create_info, &expected.allocator, &device);

    CHECK(transport_state().processed);
    CHECK(result == expected.response_result);
    CHECK(device == expected.response_device);
}

} // namespace vkfwd::forwarder::test
