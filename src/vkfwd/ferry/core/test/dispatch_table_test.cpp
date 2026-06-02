#include "generated/dispatch_table.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace vkfwd::generated {
namespace {

const VkInstance kTestInstance = reinterpret_cast<VkInstance>(0x101);
const VkDevice   kTestDevice   = reinterpret_cast<VkDevice>(0x202);

VKAPI_ATTR VkResult VKAPI_CALL fake_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *) { return VK_SUCCESS; }

VKAPI_ATTR void VKAPI_CALL fake_destroy_instance(VkInstance, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *) {
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_destroy_device(VkDevice, const VkAllocationCallbacks *) {}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(VkDevice device, const char * name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_instance_proc_addr(VkInstance instance, const char * name) {
    if (!name) { return nullptr; }
    if (instance == nullptr && std::strcmp(name, "vkCreateInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_create_instance); }
    if (instance == kTestInstance && std::strcmp(name, "vkGetDeviceProcAddr") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_get_device_proc_addr); }
    if (instance == kTestInstance && std::strcmp(name, "vkDestroyInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_instance); }
    if (instance == kTestInstance && std::strcmp(name, "vkCreateDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_create_device); }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_get_device_proc_addr(VkDevice device, const char * name) {
    if (!name) { return nullptr; }
    if (device == kTestDevice && std::strcmp(name, "vkDestroyDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_destroy_device); }
    return nullptr;
}

template<class T>
PFN_vkVoidFunction as_void_function(T function) {
    return reinterpret_cast<PFN_vkVoidFunction>(function);
}

TEST_CASE("distribution table looks up forwarded commands by generated command id") {
    DistributionTable table;
    table.global.get_instance_proc_addr = fake_get_instance_proc_addr;
    table.global.create_instance        = fake_create_instance;
    table.instance.get_device_proc_addr = fake_get_device_proc_addr;
    table.instance.destroy_instance     = fake_destroy_instance;
    table.instance.create_device        = fake_create_device;
    table.device.destroy_device         = fake_destroy_device;

    CHECK(table.getProcByCommandId(CommandId::CreateInstance) == as_void_function(fake_create_instance));
    CHECK(table.getProcByCommandId(CommandId::DestroyInstance) == as_void_function(fake_destroy_instance));
    CHECK(table.getProcByCommandId(CommandId::CreateDevice) == as_void_function(fake_create_device));
    CHECK(table.getProcByCommandId(CommandId::DestroyDevice) == as_void_function(fake_destroy_device));
}

TEST_CASE("distribution table keeps loader get-address hooks out of command-id lookup") {
    DistributionTable table;
    table.global.get_instance_proc_addr = fake_get_instance_proc_addr;
    table.instance.get_device_proc_addr = fake_get_device_proc_addr;

    // Command ids model serialized Vulkan commands. Loader discovery hooks still
    // participate in name lookup but are not commands and therefore have no
    // command-id hash entry.
    CHECK(table.getProcByName("vkGetInstanceProcAddr") == as_void_function(fake_get_instance_proc_addr));
    CHECK(table.getProcByName("vkGetDeviceProcAddr") == as_void_function(fake_get_device_proc_addr));
    CHECK(table.getProcByCommandId(static_cast<CommandId>(0)) == nullptr);
}

TEST_CASE("dispatch table init methods populate lifecycle scoped Vulkan commands") {
    GlobalDispatchTable global;
    global.init(fake_get_instance_proc_addr);
    CHECK(global.get_instance_proc_addr == fake_get_instance_proc_addr);
    CHECK(global.create_instance == fake_create_instance);

    InstanceDispatchTable instance;
    instance.init(kTestInstance, fake_get_instance_proc_addr);
    CHECK(instance.get_device_proc_addr == fake_get_device_proc_addr);
    CHECK(instance.destroy_instance == fake_destroy_instance);
    CHECK(instance.create_device == fake_create_device);

    DeviceDispatchTable device;
    device.init(kTestDevice, fake_get_device_proc_addr);
    CHECK(device.destroy_device == fake_destroy_device);
}

TEST_CASE("dispatch table init methods clear stale slots without proc address callbacks") {
    GlobalDispatchTable global;
    global.get_instance_proc_addr = fake_get_instance_proc_addr;
    global.create_instance        = fake_create_instance;
    global.init(nullptr);
    CHECK(global.get_instance_proc_addr == nullptr);
    CHECK(global.create_instance == nullptr);

    InstanceDispatchTable instance;
    instance.get_device_proc_addr = fake_get_device_proc_addr;
    instance.destroy_instance     = fake_destroy_instance;
    instance.create_device        = fake_create_device;
    instance.init(kTestInstance, nullptr);
    CHECK(instance.get_device_proc_addr == nullptr);
    CHECK(instance.destroy_instance == nullptr);
    CHECK(instance.create_device == nullptr);

    DeviceDispatchTable device;
    device.destroy_device = fake_destroy_device;
    device.init(kTestDevice, nullptr);
    CHECK(device.destroy_device == nullptr);
}

} // namespace
} // namespace vkfwd::generated
