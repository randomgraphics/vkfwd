#include "forwarder.hpp"

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#define VKFWD_EXPORT extern "C" __declspec(dllexport)
#else
#define VKFWD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct InstanceDispatch {
  // Vulkan dispatch is handle-scoped after instance creation. We cache the next
  // layer's lookup functions per instance so vkGetInstanceProcAddr can keep
  // passing unknown commands down the chain instead of terminating dispatch.
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkDestroyInstance destroy_instance = nullptr;
};

std::mutex g_instance_dispatch_mutex;
std::unordered_map<VkInstance, InstanceDispatch> g_instance_dispatch;

VkLayerInstanceCreateInfo* find_loader_link_info(
    const VkInstanceCreateInfo* create_info) {
  // The Vulkan loader passes layer chaining state through pNext during
  // vkCreateInstance. This function assumes create_info is the original
  // structure from the loader; generated interception must preserve and walk
  // pNext chains carefully because application pNext payloads and loader
  // private payloads share the same linked-list mechanism.
  auto* chain = reinterpret_cast<VkLayerInstanceCreateInfo*>(
      const_cast<void*>(create_info->pNext));

  while (chain) {
    if (chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
        chain->function == VK_LAYER_LINK_INFO) {
      return chain;
    }

    chain = reinterpret_cast<VkLayerInstanceCreateInfo*>(
        const_cast<void*>(chain->pNext));
  }

  return nullptr;
}

PFN_vkGetInstanceProcAddr lookup_next_gipa(VkInstance instance) {
  std::lock_guard lock(g_instance_dispatch_mutex);
  auto it = g_instance_dispatch.find(instance);
  if (it == g_instance_dispatch.end()) {
    return nullptr;
  }
  return it->second.get_instance_proc_addr;
}

VKAPI_ATTR VkResult VKAPI_CALL vkfwd_CreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
  // Capture happens before pass-through so failed calls are still observable.
  // The production serializer will need to deep-copy create_info, allocator
  // policy, enabled layer/extension names, and every known pNext structure.
  vkfwd::Forwarder::instance().capture({"vkCreateInstance"});

  auto* link_info = find_loader_link_info(create_info);
  if (!link_info || !link_info->u.pLayerInfo ||
      !link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  auto next_gipa = link_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  auto next = reinterpret_cast<PFN_vkCreateInstance>(
      next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!next) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  // Advancing pLayerInfo is required by the loader contract: after this layer
  // consumes its link node, the next layer must see itself as the head of the
  // chain. Failing to do this can recurse into this layer or skip downstream
  // layers.
  link_info->u.pLayerInfo = link_info->u.pLayerInfo->pNext;

  VkResult result = next(create_info, allocator, instance);
  if (result == VK_SUCCESS && instance && *instance) {
    // The dispatch table is installed only for successfully created instances.
    // This keeps lookup failure explicit for invalid or already-destroyed
    // handles and avoids preserving stale loader function pointers.
    InstanceDispatch dispatch;
    dispatch.get_instance_proc_addr = next_gipa;
    dispatch.destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        next_gipa(*instance, "vkDestroyInstance"));

    std::lock_guard lock(g_instance_dispatch_mutex);
    g_instance_dispatch.emplace(*instance, dispatch);
  }

  return result;
}

VKAPI_ATTR void VKAPI_CALL vkfwd_DestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* allocator) {
  // Destruction must be captured before removing dispatch state. Receiver-side
  // replay will depend on this ordering to tear down mapped handles after the
  // corresponding real Vulkan call has been issued.
  vkfwd::Forwarder::instance().capture({"vkDestroyInstance"});

  PFN_vkDestroyInstance next = nullptr;
  {
    std::lock_guard lock(g_instance_dispatch_mutex);
    auto it = g_instance_dispatch.find(instance);
    if (it != g_instance_dispatch.end()) {
      next = it->second.destroy_instance;
      g_instance_dispatch.erase(it);
    }
  }

  if (next) {
    next(instance, allocator);
  }
}

} // namespace

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name) {
  if (!name) {
    return nullptr;
  }

  if (std::strcmp(name, "vkCreateInstance") == 0) {
    // The loader discovers layer entry points through vkGetInstanceProcAddr.
    // Returning our wrapper here is what gives the layer its first interception
    // point before any instance-specific dispatch table exists.
    return reinterpret_cast<PFN_vkVoidFunction>(vkfwd_CreateInstance);
  }
  if (std::strcmp(name, "vkDestroyInstance") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(vkfwd_DestroyInstance);
  }

  auto next_gipa = lookup_next_gipa(instance);
  if (!next_gipa) {
    return nullptr;
  }

  return next_gipa(instance, name);
}

VKFWD_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice,
    const char*) {
  // Device dispatch is intentionally absent in the scaffold. Adding it is the
  // next major interception step because most Vulkan workload, synchronization,
  // and memory traffic enters through device-level commands.
  return nullptr;
}
