#include "generated/structure/core.hpp"

#include <cstring>
#include <new>

namespace vkfwd::generated::structure {
namespace {

template<class T>
VkResult append_shallow_struct(const T * value, Blob * blob, PackedStruct * packed) {
    if (!blob || !packed) { return VK_ERROR_UNKNOWN; }
    if (!value) {
        packed->offset = 0;
        return VK_SUCCESS;
    }
    try {
        // Vulkan typed structs put sType first. Copying sizeof(T) bytes is
        // byte-for-byte equivalent to writing sType followed by the rest of the
        // shallow struct body, and it preserves the C member offsets used for
        // pointer-slot patching below.
        packed->offset = blob->append_bytes(value, sizeof(T), alignof(T));
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
    return VK_SUCCESS;
}

template<class Pointer>
VkResult patch_pointer(Blob * blob, std::size_t structure_offset, std::size_t field_offset, Pointer target_offset) {
    const std::uintptr_t encoded = static_cast<std::uintptr_t>(target_offset);
    if (!blob->overwrite_bytes(structure_offset + field_offset, &encoded, sizeof(encoded))) { return VK_ERROR_UNKNOWN; }
    return VK_SUCCESS;
}

template<class T>
VkResult pack_plain_array(const T * values, std::uint32_t count, Blob * blob, std::size_t structure_offset, std::size_t field_offset) {
    if (count == 0 || !values) { return patch_pointer(blob, structure_offset, field_offset, 0u); }
    try {
        const std::size_t target = blob->append_bytes(values, sizeof(T) * count, alignof(T));
        return patch_pointer(blob, structure_offset, field_offset, target - structure_offset);
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

VkResult pack_string(const char * value, Blob * blob, std::size_t structure_offset, std::size_t field_offset) {
    if (!value) { return patch_pointer(blob, structure_offset, field_offset, 0u); }
    try {
        const std::size_t size   = std::strlen(value) + 1;
        const std::size_t target = blob->append_bytes(value, size, alignof(char));
        return patch_pointer(blob, structure_offset, field_offset, target - structure_offset);
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

VkResult pack_string_array(const char * const * values, std::uint32_t count, Blob * blob, std::size_t structure_offset, std::size_t field_offset) {
    if (count == 0 || !values) { return patch_pointer(blob, structure_offset, field_offset, 0u); }

    try {
        const std::size_t pointers_offset = blob->append_bytes(nullptr, 0, alignof(std::uintptr_t));
        (void) pointers_offset;
        const std::uintptr_t zero         = 0;
        const std::size_t    array_offset = blob->next_offset();
        for (std::uint32_t i = 0; i < count; ++i) { blob->append_value(zero, alignof(std::uintptr_t)); }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!values[i]) { continue; }
            const std::size_t    string_offset = blob->append_bytes(values[i], std::strlen(values[i]) + 1, alignof(char));
            const std::uintptr_t encoded       = static_cast<std::uintptr_t>(string_offset - structure_offset);
            if (!blob->overwrite_bytes(array_offset + i * sizeof(encoded), &encoded, sizeof(encoded))) { return VK_ERROR_UNKNOWN; }
        }
        return patch_pointer(blob, structure_offset, field_offset, array_offset - structure_offset);
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

template<class T>
VkResult pack_plain_typed_pnext(const T * value, Blob * blob, PackedStruct * packed) {
    VkResult status = append_shallow_struct(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, &pnext);
    if (status != VK_SUCCESS) { return status; }
    return patch_pointer(blob, packed->offset, offsetof(T, pNext), pnext.offset ? pnext.offset - packed->offset : 0);
}

template<class T>
VkResult unpack_typed_view(const Blob & blob, std::size_t offset, VkStructureType expected_stype, const T ** value) {
    if (!value) { return VK_ERROR_UNKNOWN; }
    *value             = nullptr;
    const auto * typed = reinterpret_cast<const T *>(blob.data_at(offset, sizeof(T)));
    if (!typed || typed->sType != expected_stype) { return VK_ERROR_UNKNOWN; }
    // This first generated unpack entry point validates the typed record and
    // returns the packed view. Pointer members are still offsets at this layer;
    // replay-side rehydration must resolve them with the same local base rule used
    // during pack.
    *value = typed;
    return VK_SUCCESS;
}

} // namespace

VkResult pack_VkApplicationInfo(const VkApplicationInfo * value, Blob * blob, PackedStruct * packed) {
    VkResult status = append_shallow_struct(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, &pnext);
    if (status != VK_SUCCESS) { return status; }
    status = patch_pointer(blob, packed->offset, offsetof(VkApplicationInfo, pNext), pnext.offset ? pnext.offset - packed->offset : 0);
    if (status != VK_SUCCESS) { return status; }
    status = pack_string(value->pApplicationName, blob, packed->offset, offsetof(VkApplicationInfo, pApplicationName));
    if (status != VK_SUCCESS) { return status; }
    return pack_string(value->pEngineName, blob, packed->offset, offsetof(VkApplicationInfo, pEngineName));
}

VkResult pack_VkInstanceCreateInfo(const VkInstanceCreateInfo * value, Blob * blob, PackedStruct * packed) {
    VkResult status = append_shallow_struct(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, &pnext);
    if (status != VK_SUCCESS) { return status; }
    status = patch_pointer(blob, packed->offset, offsetof(VkInstanceCreateInfo, pNext), pnext.offset ? pnext.offset - packed->offset : 0);
    if (status != VK_SUCCESS) { return status; }
    PackedStruct app;
    status = pack_VkApplicationInfo(value->pApplicationInfo, blob, &app);
    if (status != VK_SUCCESS) { return status; }
    status = patch_pointer(blob, packed->offset, offsetof(VkInstanceCreateInfo, pApplicationInfo), app.offset ? app.offset - packed->offset : 0);
    if (status != VK_SUCCESS) { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed->offset, offsetof(VkInstanceCreateInfo, ppEnabledLayerNames));
    if (status != VK_SUCCESS) { return status; }
    return pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob, packed->offset,
                             offsetof(VkInstanceCreateInfo, ppEnabledExtensionNames));
}

VkResult pack_VkDeviceQueueCreateInfo(const VkDeviceQueueCreateInfo * value, Blob * blob, PackedStruct * packed) {
    VkResult status = append_shallow_struct(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, &pnext);
    if (status != VK_SUCCESS) { return status; }
    status = patch_pointer(blob, packed->offset, offsetof(VkDeviceQueueCreateInfo, pNext), pnext.offset ? pnext.offset - packed->offset : 0);
    if (status != VK_SUCCESS) { return status; }
    return pack_plain_array(value->pQueuePriorities, value->queueCount, blob, packed->offset, offsetof(VkDeviceQueueCreateInfo, pQueuePriorities));
}

VkResult pack_VkDeviceCreateInfo(const VkDeviceCreateInfo * value, Blob * blob, PackedStruct * packed) {
    VkResult status = append_shallow_struct(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, &pnext);
    if (status != VK_SUCCESS) { return status; }
    status = patch_pointer(blob, packed->offset, offsetof(VkDeviceCreateInfo, pNext), pnext.offset ? pnext.offset - packed->offset : 0);
    if (status != VK_SUCCESS) { return status; }

    if (value->queueCreateInfoCount == 0 || !value->pQueueCreateInfos) {
        status = patch_pointer(blob, packed->offset, offsetof(VkDeviceCreateInfo, pQueueCreateInfos), 0u);
    } else {
        const std::size_t array_offset = blob->next_offset();
        for (std::uint32_t i = 0; i < value->queueCreateInfoCount; ++i) {
            PackedStruct child;
            status = pack_VkDeviceQueueCreateInfo(&value->pQueueCreateInfos[i], blob, &child);
            if (status != VK_SUCCESS) { return status; }
        }
        status = patch_pointer(blob, packed->offset, offsetof(VkDeviceCreateInfo, pQueueCreateInfos), array_offset - packed->offset);
    }
    if (status != VK_SUCCESS) { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed->offset, offsetof(VkDeviceCreateInfo, ppEnabledLayerNames));
    if (status != VK_SUCCESS) { return status; }
    status = pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob, packed->offset,
                               offsetof(VkDeviceCreateInfo, ppEnabledExtensionNames));
    if (status != VK_SUCCESS) { return status; }
    return pack_plain_array(value->pEnabledFeatures, value->pEnabledFeatures ? 1u : 0u, blob, packed->offset, offsetof(VkDeviceCreateInfo, pEnabledFeatures));
}

VkResult pack_VkDeviceGroupDeviceCreateInfo(const VkDeviceGroupDeviceCreateInfo * value, Blob * blob, PackedStruct * packed) {
    VkResult status = pack_plain_typed_pnext(value, blob, packed);
    if (status != VK_SUCCESS || !value) { return status; }
    return pack_plain_array(value->pPhysicalDevices, value->physicalDeviceCount, blob, packed->offset,
                            offsetof(VkDeviceGroupDeviceCreateInfo, pPhysicalDevices));
}

VkResult pack_VkPhysicalDeviceFeatures2(const VkPhysicalDeviceFeatures2 * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan11Features(const VkPhysicalDeviceVulkan11Features * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan12Features(const VkPhysicalDeviceVulkan12Features * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan13Features(const VkPhysicalDeviceVulkan13Features * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan14Features(const VkPhysicalDeviceVulkan14Features * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceDescriptorIndexingFeatures(const VkPhysicalDeviceDescriptorIndexingFeatures * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkDeviceQueueGlobalPriorityCreateInfo(const VkDeviceQueueGlobalPriorityCreateInfo * value, Blob * blob, PackedStruct * packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_pnext_chain(const void * value, Blob * blob, PackedStruct * packed) {
    if (!blob || !packed) { return VK_ERROR_UNKNOWN; }
    packed->offset = 0;
    if (!value) { return VK_SUCCESS; }

    const auto * base = reinterpret_cast<const VkBaseInStructure *>(value);
    switch (base->sType) {
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:
        return pack_VkDeviceGroupDeviceCreateInfo(reinterpret_cast<const VkDeviceGroupDeviceCreateInfo *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        return pack_VkPhysicalDeviceFeatures2(reinterpret_cast<const VkPhysicalDeviceFeatures2 *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        return pack_VkPhysicalDeviceVulkan11Features(reinterpret_cast<const VkPhysicalDeviceVulkan11Features *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        return pack_VkPhysicalDeviceVulkan12Features(reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        return pack_VkPhysicalDeviceVulkan13Features(reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
        return pack_VkPhysicalDeviceVulkan14Features(reinterpret_cast<const VkPhysicalDeviceVulkan14Features *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        return pack_VkPhysicalDeviceDescriptorIndexingFeatures(reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures *>(value), blob, packed);
    case VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO:
        return pack_VkDeviceQueueGlobalPriorityCreateInfo(reinterpret_cast<const VkDeviceQueueGlobalPriorityCreateInfo *>(value), blob, packed);
    default:
        // Unsupported pNext nodes are rejected instead of copied opaquely because a
        // shallow unknown struct may contain source pointers, callback functions, or
        // handle references that would be meaningless on the receiver.
        return VK_ERROR_UNKNOWN;
    }
}

VkResult unpack_VkApplicationInfo(const Blob & blob, std::size_t offset, const VkApplicationInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_APPLICATION_INFO, value);
}

VkResult unpack_VkInstanceCreateInfo(const Blob & blob, std::size_t offset, const VkInstanceCreateInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, value);
}

VkResult unpack_VkDeviceQueueCreateInfo(const Blob & blob, std::size_t offset, const VkDeviceQueueCreateInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, value);
}

VkResult unpack_VkDeviceCreateInfo(const Blob & blob, std::size_t offset, const VkDeviceCreateInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, value);
}

VkResult unpack_VkDeviceGroupDeviceCreateInfo(const Blob & blob, std::size_t offset, const VkDeviceGroupDeviceCreateInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO, value);
}

VkResult unpack_VkPhysicalDeviceFeatures2(const Blob & blob, std::size_t offset, const VkPhysicalDeviceFeatures2 ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, value);
}

VkResult unpack_VkPhysicalDeviceVulkan11Features(const Blob & blob, std::size_t offset, const VkPhysicalDeviceVulkan11Features ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, value);
}

VkResult unpack_VkPhysicalDeviceVulkan12Features(const Blob & blob, std::size_t offset, const VkPhysicalDeviceVulkan12Features ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, value);
}

VkResult unpack_VkPhysicalDeviceVulkan13Features(const Blob & blob, std::size_t offset, const VkPhysicalDeviceVulkan13Features ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, value);
}

VkResult unpack_VkPhysicalDeviceVulkan14Features(const Blob & blob, std::size_t offset, const VkPhysicalDeviceVulkan14Features ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, value);
}

VkResult unpack_VkPhysicalDeviceDescriptorIndexingFeatures(const Blob & blob, std::size_t offset, const VkPhysicalDeviceDescriptorIndexingFeatures ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, value);
}

VkResult unpack_VkDeviceQueueGlobalPriorityCreateInfo(const Blob & blob, std::size_t offset, const VkDeviceQueueGlobalPriorityCreateInfo ** value) {
    return unpack_typed_view(blob, offset, VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO, value);
}

VkResult unpack_pnext_chain(const Blob & blob, std::size_t structure_offset, const void ** value) {
    if (!value) { return VK_ERROR_UNKNOWN; }
    *value = blob.data_at(structure_offset, sizeof(VkStructureType));
    return *value ? VK_SUCCESS : VK_ERROR_UNKNOWN;
}

} // namespace vkfwd::generated::structure
