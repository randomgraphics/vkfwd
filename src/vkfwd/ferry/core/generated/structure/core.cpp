#include "generated/structure/core.hpp"

#include "logging.hpp"

#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vkfwd::generated::structure {
namespace {

template<class T>
VkResult append_shallow_struct(const T * value, Blob & blob, PackedStruct & packed, T *& packed_value) {
    packed_value = nullptr;
    if (!value) [[unlikely]] {
        packed.offset = 0;
        return VK_SUCCESS;
    }
    try {
        // Vulkan typed structs put sType first. Copying sizeof(T) bytes is
        // byte-for-byte equivalent to writing sType followed by the rest of the
        // shallow struct body, and it preserves the C member offsets used for
        // pointer-slot patching below.
        auto destination = blob.grow<T>(1);
        if (destination.set(0, *value) == false) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy shallow struct into blob, size={}, align={}", sizeof(T), alignof(T));
            return VK_ERROR_UNKNOWN;
        }
        packed.offset = destination.offset();
        packed_value  = destination.data();
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying shallow struct, size={}, align={}", sizeof(T), alignof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

template<class Pointer>
VkResult patch_pointer(Pointer & pointer_slot, std::size_t target_offset) {
    // Packed structs keep source pointer fields as relative byte offsets until
    // replay resolves them against the packed struct base. Passing the typed slot
    // makes every patch site name the Vulkan field whose ownership was copied.
    pointer_slot = reinterpret_cast<Pointer>(static_cast<std::uintptr_t>(target_offset));
    return VK_SUCCESS;
}

template<class T>
VkResult pack_plain_array(const T * values, std::uint32_t count, Blob & blob, std::size_t structure_offset, const T *& pointer_slot) {
    if (count == 0 || !values) [[unlikely]] { return patch_pointer(pointer_slot, 0u); }
    try {
        auto destination = blob.grow<T>(count);
        if (destination.set(0, count, values) != count) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy plain array into blob, count={}, element_size={}", count, sizeof(T));
            return VK_ERROR_UNKNOWN;
        }
        const std::size_t target = destination.offset();
        return patch_pointer(pointer_slot, target - structure_offset);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying plain array, count={}, element_size={}, align={}", count,
                        sizeof(T), alignof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult pack_string(const char * value, Blob & blob, std::size_t structure_offset, const char *& pointer_slot) {
    if (!value) [[unlikely]] { return patch_pointer(pointer_slot, 0u); }
    try {
        const std::size_t size        = std::strlen(value) + 1;
        auto              destination = blob.grow<char>(size);
        if (destination.set(0, size, value) != size) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy string into blob, structure_offset={}", structure_offset);
            return VK_ERROR_UNKNOWN;
        }
        const std::size_t target = destination.offset();
        return patch_pointer(pointer_slot, target - structure_offset);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying string, structure_offset={}", structure_offset);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult pack_string_array(const char * const * values, std::uint32_t count, Blob & blob, std::size_t structure_offset, const char * const *& pointer_slot) {
    if (count == 0 || !values) [[unlikely]] { return patch_pointer(pointer_slot, 0u); }

    try {
        auto                 pointer_slots = blob.grow<std::uintptr_t>(count);
        const std::size_t    array_offset  = pointer_slots.offset();
        const std::uintptr_t zero          = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!pointer_slots.set(i, zero)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not initialize string-array slot, array_offset={}, index={}", array_offset, i);
                return VK_ERROR_UNKNOWN;
            }
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!values[i]) [[unlikely]] { continue; }
            const std::size_t string_size = std::strlen(values[i]) + 1;
            auto              string_view = blob.grow<char>(string_size);
            if (string_view.set(0, string_size, values[i]) != string_size) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy string-array element, array_offset={}, index={}", array_offset, i);
                return VK_ERROR_UNKNOWN;
            }
            const std::size_t    string_offset = string_view.offset();
            const std::uintptr_t encoded       = static_cast<std::uintptr_t>(string_offset - structure_offset);
            if (!pointer_slots.set(i, encoded)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not patch string-array element, array_offset={}, index={}, string_offset={}",
                                array_offset, i, string_offset);
                return VK_ERROR_UNKNOWN;
            }
        }
        return patch_pointer(pointer_slot, array_offset - structure_offset);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying string array, count={}, structure_offset={}", count,
                        structure_offset);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

template<class T>
VkResult pack_plain_typed_pnext(const T * value, Blob & blob, PackedStruct & packed, T *& packed_value) {
    VkResult status = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return patch_pointer(packed_value->pNext, pnext.offset ? pnext.offset - packed.offset : 0);
}

template<class T>
VkResult pack_plain_typed_pnext(const T * value, Blob & blob, PackedStruct & packed) {
    T * packed_value = nullptr;
    return pack_plain_typed_pnext(value, blob, packed, packed_value);
}

template<class T>
VkResult unpack_typed_view(const Blob & blob, std::size_t offset, VkStructureType expected_stype, const T ** value) {
    if (!value) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: output pointer for typed view is null, offset={}, expected_sType={}", offset,
                        static_cast<int>(expected_stype));
        return VK_ERROR_UNKNOWN;
    }
    *value                  = nullptr;
    const auto   typed_view = blob.data_at(offset, sizeof(T));
    const auto * typed      = reinterpret_cast<const T *>(typed_view.data());
    if (!typed) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: blob does not contain typed view, offset={}, size={}, expected_sType={}", offset, sizeof(T),
                        static_cast<int>(expected_stype));
        return VK_ERROR_UNKNOWN;
    }
    if (typed->sType != expected_stype) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: sType mismatch, offset={}, expected_sType={}, actual_sType={}", offset,
                        static_cast<int>(expected_stype), static_cast<int>(typed->sType));
        return VK_ERROR_UNKNOWN;
    }
    // This first generated unpack entry point validates the typed record and
    // returns the packed view. Pointer members are still offsets at this layer;
    // replay-side rehydration must resolve them with the same local base rule used
    // during pack.
    *value = typed;
    return VK_SUCCESS;
}

using GenericPackFn = VkResult (*)(const void *, Blob &, PackedStruct &);

template<class T>
VkResult pack_struct_as(const void * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(reinterpret_cast<const T *>(value), blob, packed);
}

VkResult pack_device_group_device_create_info_as(const void * value, Blob & blob, PackedStruct & packed) {
    return pack_VkDeviceGroupDeviceCreateInfo(reinterpret_cast<const VkDeviceGroupDeviceCreateInfo *>(value), blob, packed);
}

#if defined(__unix__) || defined(__APPLE__)
thread_local sigjmp_buf * g_active_fault_probe = nullptr;

std::mutex & fault_probe_mutex() {
    static std::mutex mutex;
    return mutex;
}

void handle_fault_probe_signal(int signum, siginfo_t *, void *) {
    if (g_active_fault_probe) { siglongjmp(*g_active_fault_probe, 1); }
    std::_Exit(128 + signum);
}

bool copy_from_application_memory(const void * source, void * destination, std::size_t size) {
    if (!source || !destination || size == 0) [[unlikely]] { return false; }

    std::lock_guard lock(fault_probe_mutex());

    struct sigaction action {};
    action.sa_sigaction = handle_fault_probe_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO;

    struct sigaction previous_segv {};
    struct sigaction previous_bus {};
    if (sigaction(SIGSEGV, &action, &previous_segv) != 0) [[unlikely]] { return false; }
    if (sigaction(SIGBUS, &action, &previous_bus) != 0) [[unlikely]] {
        sigaction(SIGSEGV, &previous_segv, nullptr);
        return false;
    }

    sigjmp_buf jump_buffer;
    g_active_fault_probe            = &jump_buffer;
    volatile sig_atomic_t did_copy  = 0;
    volatile sig_atomic_t did_fault = 0;
    if (sigsetjmp(jump_buffer, 1) == 0) {
        std::memcpy(destination, source, size);
        did_copy = 1;
    } else {
        did_fault = 1;
    }
    g_active_fault_probe = nullptr;

    sigaction(SIGBUS, &previous_bus, nullptr);
    sigaction(SIGSEGV, &previous_segv, nullptr);
    return did_copy != 0 && did_fault == 0;
}
#else
bool copy_from_application_memory(const void * source, void * destination, std::size_t size) {
    if (!source || !destination || size == 0) [[unlikely]] { return false; }
    std::memcpy(destination, source, size);
    return true;
}
#endif

template<class T>
bool copy_from_application_memory(const void * source, T & destination) {
    return copy_from_application_memory(source, &destination, sizeof(T));
}

std::size_t pnext_node_size(VkStructureType type) {
    switch (type) {
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:
        return sizeof(VkDeviceGroupDeviceCreateInfo);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        return sizeof(VkPhysicalDeviceFeatures2);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan11Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan12Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan13Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES:
        return sizeof(VkPhysicalDeviceVulkan14Features);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        return sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);
    case VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO:
        return sizeof(VkDeviceQueueGlobalPriorityCreateInfo);
    default:
        return 0;
    }
}

VkResult validate_pnext_node_readable(const void * value, VkStructureType type, std::size_t depth) {
    const std::size_t node_size = pnext_node_size(type);
    if (node_size == 0) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: no size for sType={}, depth={}, node={}", static_cast<int>(type), depth, value);
        return VK_ERROR_UNKNOWN;
    }

    try {
        // pNext nodes are borrowed application memory. Probing the whole known
        // node before any generated packer copies it keeps corrupt chains from
        // turning a validation failure into a process fault.
        std::vector<std::byte> scratch(node_size);
        if (!copy_from_application_memory(value, scratch.data(), scratch.size())) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: unreadable node memory, sType={}, depth={}, node={}, size={}", static_cast<int>(type), depth,
                            value, node_size);
            return VK_ERROR_UNKNOWN;
        }
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: out of host memory while probing node, sType={}, depth={}, node={}", static_cast<int>(type),
                        depth, value);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    return VK_SUCCESS;
}

const std::unordered_map<VkStructureType, GenericPackFn> & generic_packers() {
    // The fallback map is intentionally local: switch dispatch remains the fast
    // path for currently generated pNext structs, while this table keeps type
    // based packing extensible for generated cases that are not hand-spelled in
    // pack_pnext_chain(). All entries still copy known structs only; unknown
    // payloads are rejected because their pointer ownership is not described.
    static const std::unordered_map<VkStructureType, GenericPackFn> packers = {
        {VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO, pack_device_group_device_create_info_as},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, pack_struct_as<VkPhysicalDeviceFeatures2>},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, pack_struct_as<VkPhysicalDeviceVulkan11Features>},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, pack_struct_as<VkPhysicalDeviceVulkan12Features>},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, pack_struct_as<VkPhysicalDeviceVulkan13Features>},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, pack_struct_as<VkPhysicalDeviceVulkan14Features>},
        {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, pack_struct_as<VkPhysicalDeviceDescriptorIndexingFeatures>},
        {VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO, pack_struct_as<VkDeviceQueueGlobalPriorityCreateInfo>},
    };
    return packers;
}

VkResult validate_pnext_chain(const void * value) {
    constexpr std::size_t            kMaxPnextDepth = 1000;
    std::unordered_set<const void *> seen;

    try {
        const auto & packers = generic_packers();
        for (std::size_t depth = 0; value; ++depth) {
            // pNext is borrowed application memory during interception. Validate
            // the whole chain before dumping any node so replay never receives a
            // partial chain whose ordering, termination, or known-type contract
            // was already suspect at the source boundary.
            if (depth >= kMaxPnextDepth) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: chain depth exceeded limit, limit={}", kMaxPnextDepth);
                return VK_ERROR_UNKNOWN;
            }
            if (!seen.insert(value).second) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: loop detected at depth={}, node={}", depth, value);
                return VK_ERROR_UNKNOWN;
            }

            VkBaseInStructure base {};
            if (!copy_from_application_memory(value, base)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: unreadable node header, depth={}, node={}", depth, value);
                return VK_ERROR_UNKNOWN;
            }
            if (!packers.contains(base.sType)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: unsupported sType={}, depth={}, node={}", static_cast<int>(base.sType), depth, value);
                return VK_ERROR_UNKNOWN;
            }
            VkResult status = validate_pnext_node_readable(value, base.sType, depth);
            if (status != VK_SUCCESS) [[unlikely]] { return status; }
            value = base.pNext;
        }
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry pNext validation failed: out of host memory while tracking visited nodes");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    return VK_SUCCESS;
}

} // namespace

VkResult pack_VkApplicationInfo(const VkApplicationInfo * value, Blob & blob, PackedStruct & packed) {
    VkApplicationInfo * packed_value = nullptr;
    VkResult            status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, pnext.offset ? pnext.offset - packed.offset : 0);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string(value->pApplicationName, blob, packed.offset, packed_value->pApplicationName);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_string(value->pEngineName, blob, packed.offset, packed_value->pEngineName);
}

VkResult pack_VkInstanceCreateInfo(const VkInstanceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkInstanceCreateInfo * packed_value = nullptr;
    VkResult               status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, pnext.offset ? pnext.offset - packed.offset : 0);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    PackedStruct app;
    status = pack_VkApplicationInfo(value->pApplicationInfo, blob, app);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pApplicationInfo, app.offset ? app.offset - packed.offset : 0);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed.offset, packed_value->ppEnabledLayerNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob, packed.offset, packed_value->ppEnabledExtensionNames);
}

VkResult pack_VkDeviceQueueCreateInfo(const VkDeviceQueueCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceQueueCreateInfo * packed_value = nullptr;
    VkResult                  status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, pnext.offset ? pnext.offset - packed.offset : 0);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_plain_array(value->pQueuePriorities, value->queueCount, blob, packed.offset, packed_value->pQueuePriorities);
}

VkResult pack_VkDeviceCreateInfo(const VkDeviceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceCreateInfo * packed_value = nullptr;
    VkResult             status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, pnext.offset ? pnext.offset - packed.offset : 0);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }

    if (value->queueCreateInfoCount == 0 || !value->pQueueCreateInfos) [[unlikely]] {
        status = patch_pointer(packed_value->pQueueCreateInfos, 0u);
    } else {
        const std::size_t array_offset = blob.size();
        for (std::uint32_t i = 0; i < value->queueCreateInfoCount; ++i) {
            PackedStruct child;
            status = pack_VkDeviceQueueCreateInfo(&value->pQueueCreateInfos[i], blob, child);
            if (status != VK_SUCCESS) [[unlikely]] { return status; }
        }
        status = patch_pointer(packed_value->pQueueCreateInfos, array_offset - packed.offset);
    }
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed.offset, packed_value->ppEnabledLayerNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob, packed.offset, packed_value->ppEnabledExtensionNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_plain_array(value->pEnabledFeatures, value->pEnabledFeatures ? 1u : 0u, blob, packed.offset, packed_value->pEnabledFeatures);
}

VkResult pack_VkDeviceGroupDeviceCreateInfo(const VkDeviceGroupDeviceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceGroupDeviceCreateInfo * packed_value = nullptr;
    VkResult                        status       = pack_plain_typed_pnext(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    return pack_plain_array(value->pPhysicalDevices, value->physicalDeviceCount, blob, packed.offset, packed_value->pPhysicalDevices);
}

VkResult pack_VkPhysicalDeviceFeatures2(const VkPhysicalDeviceFeatures2 * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan11Features(const VkPhysicalDeviceVulkan11Features * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan12Features(const VkPhysicalDeviceVulkan12Features * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan13Features(const VkPhysicalDeviceVulkan13Features * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceVulkan14Features(const VkPhysicalDeviceVulkan14Features * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkPhysicalDeviceDescriptorIndexingFeatures(const VkPhysicalDeviceDescriptorIndexingFeatures * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_VkDeviceQueueGlobalPriorityCreateInfo(const VkDeviceQueueGlobalPriorityCreateInfo * value, Blob & blob, PackedStruct & packed) {
    return pack_plain_typed_pnext(value, blob, packed);
}

VkResult pack_pnext_chain(const void * value, Blob & blob, PackedStruct & packed) {
    packed.offset = 0;
    if (!value) [[likely]] { return VK_SUCCESS; }

    VkResult status = validate_pnext_chain(value);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }

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
        return pack_struct_by_type(value, blob, packed);
    }
}

VkResult pack_struct_by_type(const void * value, Blob & blob, PackedStruct & packed) {
    packed.offset = 0;
    if (!value) [[unlikely]] { return VK_SUCCESS; }

    try {
        const auto * base    = reinterpret_cast<const VkBaseInStructure *>(value);
        const auto & packers = generic_packers();
        const auto   found   = packers.find(base->sType);
        if (found == packers.end()) [[unlikely]] {
            // Unknown typed structs are rejected instead of copied opaquely because a
            // shallow unknown struct may contain source pointers, callback functions,
            // or handle references that would be meaningless on the receiver.
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: no generic packer for sType={}", static_cast<int>(base->sType));
            return VK_ERROR_UNKNOWN;
        }
        return found->second(value, blob, packed);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while looking up generic packer");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
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
    if (!value) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry pNext unpack failed: output pointer is null, structure_offset={}", structure_offset);
        return VK_ERROR_UNKNOWN;
    }
    const auto node_header = blob.data_at(structure_offset, sizeof(VkStructureType));
    *value                 = node_header.data();
    if (!*value) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry pNext unpack failed: blob does not contain pNext node header, structure_offset={}", structure_offset);
        return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

} // namespace vkfwd::generated::structure
