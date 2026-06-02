#pragma once

// Generated structure pack/unpack slice; do not edit by hand.
// Vulkan API version: 1.4.352

#include "blob.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>

namespace vkfwd::generated::structure {

struct PackedStruct {
    std::size_t offset = 0;
};

VkResult pack_VkApplicationInfo(const VkApplicationInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkInstanceCreateInfo(const VkInstanceCreateInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkDeviceQueueCreateInfo(const VkDeviceQueueCreateInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkDeviceCreateInfo(const VkDeviceCreateInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkDeviceGroupDeviceCreateInfo(const VkDeviceGroupDeviceCreateInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceFeatures2(const VkPhysicalDeviceFeatures2 * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceVulkan11Features(const VkPhysicalDeviceVulkan11Features * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceVulkan12Features(const VkPhysicalDeviceVulkan12Features * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceVulkan13Features(const VkPhysicalDeviceVulkan13Features * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceVulkan14Features(const VkPhysicalDeviceVulkan14Features * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkPhysicalDeviceDescriptorIndexingFeatures(const VkPhysicalDeviceDescriptorIndexingFeatures * value, Blob & blob, PackedStruct & packed);
VkResult pack_VkDeviceQueueGlobalPriorityCreateInfo(const VkDeviceQueueGlobalPriorityCreateInfo * value, Blob & blob, PackedStruct & packed);
VkResult pack_struct_by_type(const void * value, Blob & blob, PackedStruct & packed);
VkResult pack_pnext_chain(const void * value, Blob & blob, PackedStruct & packed);

VkResult unpack_VkApplicationInfo(SafeArrayView<std::uint8_t> & view, const VkApplicationInfo ** value);
VkResult unpack_VkInstanceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkInstanceCreateInfo ** value);
VkResult unpack_VkDeviceQueueCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceQueueCreateInfo ** value);
VkResult unpack_VkDeviceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceCreateInfo ** value);
VkResult unpack_VkDeviceGroupDeviceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceGroupDeviceCreateInfo ** value);
VkResult unpack_VkPhysicalDeviceFeatures2(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceFeatures2 ** value);
VkResult unpack_VkPhysicalDeviceVulkan11Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan11Features ** value);
VkResult unpack_VkPhysicalDeviceVulkan12Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan12Features ** value);
VkResult unpack_VkPhysicalDeviceVulkan13Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan13Features ** value);
VkResult unpack_VkPhysicalDeviceVulkan14Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan14Features ** value);
VkResult unpack_VkPhysicalDeviceDescriptorIndexingFeatures(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceDescriptorIndexingFeatures ** value);
VkResult unpack_VkDeviceQueueGlobalPriorityCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceQueueGlobalPriorityCreateInfo ** value);
VkResult unpack_pnext_chain(SafeArrayView<std::uint8_t> & view, const void ** value);

} // namespace vkfwd::generated::structure
