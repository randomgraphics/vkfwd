#include "../sample/loopback_runtime.hpp"
#include "forwarder.hpp"
#include "generated/forwarder_entrypoints.hpp"

#define RAPID_VULKAN_IMPLEMENTATION 1
#include <rapid-vulkan/rapid-vulkan.h>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace vkfwd::test {
namespace {

constexpr VkInstanceCreateFlags kExpectedFlags    = 0xbad;
const auto                      kReceiverInstance = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xbeef));

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateInstance(const VkInstanceCreateInfo * pCreateInfo, const VkAllocationCallbacks *, VkInstance * pInstance) {
    REQUIRE(pCreateInfo != nullptr);
    CHECK(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    CHECK(pCreateInfo->flags == kExpectedFlags);
    REQUIRE(pInstance != nullptr);
    *pInstance = kReceiverInstance;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkCreateInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateInstance); }
    return nullptr;
}

const auto kReceiverPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xd1ce));
const auto kReceiverDevice         = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xde71ce));

// Set by fake_vkDeviceWaitIdle. A device-level command only reaches its host
// function pointer if receiver-side device dispatch was initialized by the
// vkCreateDevice after_call hook, so observing this flag proves the hook fired.
bool g_device_wait_idle_called = false;

VKAPI_ATTR VkResult VKAPI_CALL fake_vkDeviceWaitIdle(VkDevice device) {
    CHECK(device == kReceiverDevice);
    g_device_wait_idle_called = true;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetDeviceProcAddr(VkDevice, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkDeviceWaitIdle") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDeviceWaitIdle); }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks *,
                                                   VkDevice * pDevice) {
    REQUIRE(pCreateInfo != nullptr);
    CHECK(pCreateInfo->sType == VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    REQUIRE(pDevice != nullptr);
    *pDevice = kReceiverDevice;
    return VK_SUCCESS;
}

// Separate from fake_vkGetInstanceProcAddr so the existing single-call test
// keeps its instance-only loader; this one also resolves the device-creation
// chain the receiver needs to initialize instance and device dispatch.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_device_capable_vkGetInstanceProcAddr(VkInstance, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkCreateInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateInstance); }
    if (std::strcmp(name, "vkCreateDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateDevice); }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetDeviceProcAddr); }
    return nullptr;
}

void create_and_destroy_rapid_vulkan_instance(PFN_vkGetInstanceProcAddr get_instance_proc_addr) {
    rapid_vulkan::Instance::ConstructParameters cp;
    cp.setValidation(rapid_vulkan::Instance::VALIDATION_DISABLED).setPrintVkInfo(rapid_vulkan::Device::SILENCE);
    cp.getInstanceProcAddr = get_instance_proc_addr;

    rapid_vulkan::Instance instance(cp);
    if (!instance.handle()) { throw std::runtime_error("rapid-vulkan created a null VkInstance"); }
}

} // namespace

TEST_CASE("loopback forwards vkCreateInstance through receiver replay", "[loopback]") {
    // The fake host loader keeps this as a single-call replay contract while
    // still using the same loopback runtime as the sample.
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    VkInstanceCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags = kExpectedFlags,
    };
    VkInstance instance = VK_NULL_HANDLE;

    const VkResult result = forwarder::generated::vkCreateInstance_entry(&create_info, nullptr, &instance);

    CHECK(result == VK_SUCCESS);
    CHECK(instance == kReceiverInstance);
}

TEST_CASE("loopback replays a device-level command via receiver dispatch hooks", "[loopback]") {
    // Forwarding vkDeviceWaitIdle only succeeds if the receiver initialized
    // instance dispatch (vkCreateInstance after_call hook) and then device
    // dispatch (vkCreateDevice after_call hook). This is the regression guard
    // for those two hand-written receiver hooks.
    g_device_wait_idle_called = false;
    sample::VkfwdLoopbackRuntime vkfwd(fake_device_capable_vkGetInstanceProcAddr);

    // fake_vkCreateInstance asserts flags == kExpectedFlags, so match it here.
    VkInstanceCreateInfo instance_create_info {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .flags = kExpectedFlags};
    VkInstance           instance = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateInstance_entry(&instance_create_info, nullptr, &instance) == VK_SUCCESS);
    REQUIRE(instance == kReceiverInstance);

    VkDeviceCreateInfo device_create_info {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice           device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateDevice_entry(kReceiverPhysicalDevice, &device_create_info, nullptr, &device) == VK_SUCCESS);
    REQUIRE(device == kReceiverDevice);

    const VkResult wait_result = forwarder::generated::vkDeviceWaitIdle_entry(device);
    CHECK(wait_result == VK_SUCCESS);
    CHECK(g_device_wait_idle_called);
}

TEST_CASE("rapid-vulkan instance smoke passes direct and with vkfwd loopback", "[loopback][rapid-vulkan][hardware]") {
    try {
        create_and_destroy_rapid_vulkan_instance(nullptr);
    } catch (const std::exception & e) { SKIP(std::string("Direct rapid-vulkan instance creation is unavailable: ") + e.what()); }

    std::unique_ptr<::vkfwd::sample::VkfwdLoopbackRuntime> vkfwd;
    try {
        vkfwd = std::make_unique<::vkfwd::sample::VkfwdLoopbackRuntime>();
    } catch (const std::exception & e) { SKIP(std::string("vkfwd loopback runtime is unavailable: ") + e.what()); }

    create_and_destroy_rapid_vulkan_instance(::vkfwd::Forwarder::getInstanceProcAddr);
}

} // namespace vkfwd::test
