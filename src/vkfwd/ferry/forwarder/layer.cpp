#include "generated/vulkan_forwarder.hpp"

#include <vulkan/vulkan.h>

#include <cstring>

#if defined(_WIN32)
#define VKFWD_EXPORT extern "C" __declspec(dllexport)
#else
#define VKFWD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name);
VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* name);

namespace {

PFN_vkVoidFunction lookup_global_entrypoint(const char* name) {
  const auto& table = vkfwd::forwarder::generated::global_dispatch_table();
  if (std::strcmp(name, "vkCreateInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(table.create_instance);
  }
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetInstanceProcAddr);
  }
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkGetDeviceProcAddr);
  }
  return nullptr;
}

PFN_vkVoidFunction lookup_instance_entrypoint(const char* name) {
  const auto& table = vkfwd::forwarder::generated::instance_dispatch_table();
  if (std::strcmp(name, "vkDestroyInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(table.destroy_instance);
  }
  if (std::strcmp(name, "vkCreateDevice") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(table.create_device);
  }
  return nullptr;
}

PFN_vkVoidFunction lookup_device_entrypoint(const char* name) {
  const auto& table = vkfwd::forwarder::generated::device_dispatch_table();
  if (std::strcmp(name, "vkDestroyDevice") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(table.destroy_device);
  }
  return nullptr;
}

} // namespace

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name) {
  if (!name) {
    return nullptr;
  }

  // The source forwarder has one shared dispatch table per command level. The
  // table is independent of instance identity because every entry points to a
  // generated pack/forward wrapper, never to a local driver or lower layer.
  if (auto entrypoint = lookup_global_entrypoint(name)) {
    return entrypoint;
  }
  if (auto entrypoint = lookup_instance_entrypoint(name)) {
    return entrypoint;
  }

  // Unknown commands remain unavailable until vkfwd owns their generated pack,
  // endpoint response, and output-parameter contract.
  (void)instance;
  return nullptr;
}

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device,
    const char* name) {
  if (!name) {
    return nullptr;
  }

  if (auto entrypoint = lookup_global_entrypoint(name)) {
    return entrypoint;
  }
  if (auto entrypoint = lookup_device_entrypoint(name)) {
    return entrypoint;
  }

  // Device lookup follows the same forwarder invariant: no command pointer is
  // exposed unless it is backed by a vkfwd generated entrypoint.
  (void)device;
  return nullptr;
}
