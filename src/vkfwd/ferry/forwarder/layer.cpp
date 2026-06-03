#include "forwarder.hpp"

namespace {

#if defined(_WIN32)
    #define VKFWD_EXPORT extern "C" __declspec(dllexport)
#else
    #define VKFWD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char * name) {
    return vkfwd::Forwarder::getInstanceProcAddr(instance, name);
}

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char * name) {
    return vkfwd::Forwarder::getDeviceProcAddr(device, name);
}

} // namespace