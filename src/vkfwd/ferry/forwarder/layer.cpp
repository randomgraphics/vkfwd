#include "generated/dispatch_table.hpp"
#include "generated/forwarder_entrypoints.hpp"

#if defined(_WIN32)
    #define VKFWD_EXPORT extern "C" __declspec(dllexport)
#else
    #define VKFWD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char * name);
VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char * name);

namespace {

PFN_vkVoidFunction lookup_global_entrypoint(const char * name) { return vkfwd::forwarder::generated::global_dispatch_table().getProcByName(name); }

PFN_vkVoidFunction lookup_instance_entrypoint(const char * name) { return vkfwd::forwarder::generated::instance_dispatch_table().getProcByName(name); }

PFN_vkVoidFunction lookup_device_entrypoint(const char * name) { return vkfwd::forwarder::generated::device_dispatch_table().getProcByName(name); }

} // namespace

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    if (!name) { return nullptr; }

    // Global commands are available before a VkInstance exists; instance
    // commands become discoverable through the same loader hook once the
    // application has an instance. Both tables point to vkfwd wrappers, never to
    // a local driver or lower layer.
    if (auto entrypoint = lookup_global_entrypoint(name)) { return entrypoint; }
    if (auto entrypoint = lookup_instance_entrypoint(name)) { return entrypoint; }

    // Unknown commands remain unavailable until vkfwd owns their generated pack,
    // response payload, and output-parameter contract.
    (void) instance;
    return nullptr;
}

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char * name) {
    if (!name) { return nullptr; }

    if (auto entrypoint = lookup_device_entrypoint(name)) { return entrypoint; }

    // Device lookup follows the same forwarder invariant: no command pointer is
    // exposed unless it is backed by a vkfwd generated entrypoint.
    (void) device;
    return nullptr;
}
