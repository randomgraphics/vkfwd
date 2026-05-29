#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace vkfwd::wire_1_0 {

constexpr std::uint16_t kWireMajor = 1;
constexpr std::uint16_t kWireMinor = 0;
constexpr bool kIsPubliclyReleased = false;
constexpr const char* kPublicReleaseTimestamp = "";

// This header defines wire revision 1.0 packet ownership behavior. While
// kIsPubliclyReleased is false, this file may change without a version bump.
// Once published, incompatible changes to copied layout, pointer
// reconstruction, pNext handling, failure semantics, or supported pNext structs
// must move to a new wire_<major>_<minor>.hpp file instead of editing this one.
// kPublicReleaseTimestamp is informational history/debug metadata only; no
// runtime compatibility decision should depend on it.

struct PNextNode {
  VkStructureType sType = VK_STRUCTURE_TYPE_MAX_ENUM;
  std::vector<std::byte> bytes;
};

template <class T>
VkResult copy_typed_pointer(const T* source, T* destination) {
  if (!source) {
    return VK_SUCCESS;
  }
  if (!destination) {
    return VK_ERROR_UNKNOWN;
  }
  *destination = *source;
  return VK_SUCCESS;
}

template <class T>
VkResult copy_sized_array(uint32_t count,
                          const T* source,
                          std::vector<T>* destination) {
  if (!destination) {
    return VK_ERROR_UNKNOWN;
  }
  destination->clear();
  if (count == 0) {
    return VK_SUCCESS;
  }
  if (!source) {
    return VK_ERROR_UNKNOWN;
  }
  try {
    destination->assign(source, source + count);
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return VK_SUCCESS;
}

inline VkResult copy_string_array(uint32_t count,
                                  const char* const* source,
                                  std::vector<std::string>* strings,
                                  std::vector<const char*>* pointers) {
  if (!strings || !pointers) {
    return VK_ERROR_UNKNOWN;
  }
  strings->clear();
  pointers->clear();
  if (count == 0) {
    return VK_SUCCESS;
  }
  if (!source) {
    return VK_ERROR_UNKNOWN;
  }
  try {
    strings->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      strings->push_back(source[i] ? source[i] : "");
    }
    pointers->reserve(strings->size());
    for (const auto& value : *strings) {
      pointers->push_back(value.c_str());
    }
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  return VK_SUCCESS;
}

template <class T>
VkResult copy_pnext_node(const void* source, PNextNode* node) {
  if (!source || !node) {
    return VK_ERROR_UNKNOWN;
  }
  try {
    node->bytes.resize(sizeof(T));
  } catch (const std::bad_alloc&) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  std::memcpy(node->bytes.data(), source, sizeof(T));
  node->sType = reinterpret_cast<const VkBaseInStructure*>(source)->sType;
  return VK_SUCCESS;
}

inline VkResult copy_pnext_chain(const void* source,
                                 std::vector<PNextNode>* nodes) {
  if (!nodes) {
    return VK_ERROR_UNKNOWN;
  }
  nodes->clear();
  for (auto* current = reinterpret_cast<const VkBaseInStructure*>(source);
       current; current = current->pNext) {
    PNextNode node;
    VkResult status = VK_SUCCESS;
    switch (current->sType) {
    case VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT:
      status =
          copy_pnext_node<VkDebugUtilsMessengerCreateInfoEXT>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT:
      status = copy_pnext_node<VkValidationFeaturesEXT>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT:
      status = copy_pnext_node<VkValidationFlagsEXT>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT:
      status = copy_pnext_node<VkLayerSettingsCreateInfoEXT>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:
      status = copy_pnext_node<VkDeviceGroupDeviceCreateInfo>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
      status = copy_pnext_node<VkPhysicalDeviceFeatures2>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
      status =
          copy_pnext_node<VkPhysicalDeviceVulkan11Features>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
      status =
          copy_pnext_node<VkPhysicalDeviceVulkan12Features>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
      status =
          copy_pnext_node<VkPhysicalDeviceVulkan13Features>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
      status =
          copy_pnext_node<VkPhysicalDeviceVulkan14Features>(current, &node);
      break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
      status = copy_pnext_node<VkPhysicalDeviceDescriptorIndexingFeatures>(
          current, &node);
      break;
    case VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO:
      status =
          copy_pnext_node<VkDeviceQueueGlobalPriorityCreateInfo>(current, &node);
      break;
    default:
      return VK_ERROR_UNKNOWN;
    }
    if (status != VK_SUCCESS) {
      return status;
    }
    try {
      nodes->push_back(std::move(node));
    } catch (const std::bad_alloc&) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  return VK_SUCCESS;
}

inline const void* rebuild_pnext_chain(std::vector<PNextNode>* nodes) {
  const void* next = nullptr;
  for (auto it = nodes->rbegin(); it != nodes->rend(); ++it) {
    auto* base = reinterpret_cast<VkBaseOutStructure*>(it->bytes.data());
    base->pNext =
        reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(next));
    next = it->bytes.data();
  }
  return next;
}

} // namespace vkfwd::wire_1_0
