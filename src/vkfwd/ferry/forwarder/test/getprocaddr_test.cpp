#include "generated/forwarder_entrypoints.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char * name);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char * name);

namespace vkfwd::forwarder::test {
namespace {

template<class FunctionPointer>
PFN_vkVoidFunction as_void(FunctionPointer function) {
    return reinterpret_cast<PFN_vkVoidFunction>(function);
}

} // namespace

TEST_CASE("vkGetInstanceProcAddr follows Vulkan dispatch lookup shape for vkfwd-owned commands") {
    const auto instance = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0x101));
    const auto device   = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x202));

    CHECK(::vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance") == as_void(generated::vkCreateInstance_entry));
    CHECK(::vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyInstance") == nullptr);
    CHECK(::vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyDevice") == nullptr);

    CHECK(::vkGetInstanceProcAddr(instance, "vkDestroyInstance") == as_void(generated::vkDestroyInstance_entry));
    CHECK(::vkGetInstanceProcAddr(instance, "vkCreateDevice") == as_void(generated::vkCreateDevice_entry));
    // Vulkan permits vkGetInstanceProcAddr(instance, ...) to return dispatchable
    // device-command trampolines. Some loaders, including Vulkan-Hpp dynamic
    // dispatch users, use this path for device commands before asking
    // vkGetDeviceProcAddr for device-specific shortcuts.
    CHECK(::vkGetInstanceProcAddr(instance, "vkDestroyDevice") == as_void(generated::vkDestroyDevice_entry));
    CHECK(::vkGetDeviceProcAddr(device, "vkDestroyDevice") == as_void(generated::vkDestroyDevice_entry));

    CHECK(::vkGetInstanceProcAddr(instance, "vkCreateBuffer") == nullptr);
    CHECK(::vkGetDeviceProcAddr(device, "vkCreateBuffer") == nullptr);
}

} // namespace vkfwd::forwarder::test
