#include "generated/dispatch_table.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::generated {
namespace {

VKAPI_ATTR VkResult VKAPI_CALL fake_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *) { return VK_SUCCESS; }

VKAPI_ATTR void VKAPI_CALL fake_destroy_instance(VkInstance, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_device(VkDevice, const VkAllocationCallbacks *) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_instance_proc_addr(VkInstance, const char *) { return nullptr; }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(VkDevice, const char *) { return nullptr; }

template<class T>
PFN_vkVoidFunction as_void_function(T function) {
    return reinterpret_cast<PFN_vkVoidFunction>(function);
}

TEST_CASE("distribution table looks up forwarded commands by generated command id") {
    DistributionTable table;
    table.instance.get_instance_proc_addr = fake_get_instance_proc_addr;
    table.instance.get_device_proc_addr   = fake_get_device_proc_addr;
    table.instance.create_instance        = fake_create_instance;
    table.instance.destroy_instance       = fake_destroy_instance;
    table.instance.create_device          = fake_create_device;
    table.device.destroy_device           = fake_destroy_device;

    CHECK(table.getProcByCommandId(CommandId::CreateInstance) == as_void_function(fake_create_instance));
    CHECK(table.getProcByCommandId(CommandId::DestroyInstance) == as_void_function(fake_destroy_instance));
    CHECK(table.getProcByCommandId(CommandId::CreateDevice) == as_void_function(fake_create_device));
    CHECK(table.getProcByCommandId(CommandId::DestroyDevice) == as_void_function(fake_destroy_device));
}

TEST_CASE("distribution table keeps loader get-address hooks out of command-id lookup") {
    DistributionTable table;
    table.instance.get_instance_proc_addr = fake_get_instance_proc_addr;
    table.instance.get_device_proc_addr   = fake_get_device_proc_addr;

    // Command ids model serialized Vulkan commands. Loader discovery hooks still
    // participate in name lookup but are not commands and therefore have no
    // command-id hash entry.
    CHECK(table.getProcByName("vkGetInstanceProcAddr") == as_void_function(fake_get_instance_proc_addr));
    CHECK(table.getProcByName("vkGetDeviceProcAddr") == as_void_function(fake_get_device_proc_addr));
    CHECK(table.getProcByCommandId(static_cast<CommandId>(0)) == nullptr);
}

} // namespace
} // namespace vkfwd::generated
