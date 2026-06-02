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
        std::size_t destination_offset = 0;
        auto        destination        = blob.grow<T>(1, alignof(T), &destination_offset);
        if (destination.set(0, *value) == false) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy shallow struct into blob, size={}, align={}", sizeof(T), alignof(T));
            return VK_ERROR_UNKNOWN;
        }
        packed.offset = destination_offset;
        packed_value  = &destination.at(0);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying shallow struct, size={}, align={}", sizeof(T), alignof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

template<class Pointer>
VkResult patch_pointer(Pointer & pointer_slot, std::size_t pointer_slot_offset, std::size_t target_offset) {
    // Every encoded pointer is relative to the field that stores it. That single
    // base rule lets unpack repair the graph in-place from the pointer slot's own
    // address without carrying parent-structure offsets through replay code.
    pointer_slot = reinterpret_cast<Pointer>(target_offset ? static_cast<std::uintptr_t>(target_offset - pointer_slot_offset) : 0);
    return VK_SUCCESS;
}

template<class T>
VkResult pack_plain_array(const T * values, std::uint32_t count, Blob & blob, std::size_t pointer_slot_offset, const T *& pointer_slot) {
    if (count == 0 || !values) [[unlikely]] { return patch_pointer(pointer_slot, pointer_slot_offset, 0u); }
    try {
        std::size_t target      = 0;
        auto        destination = blob.grow<T>(count, alignof(T), &target);
        if (destination.set(0, count, values) != count) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy plain array into blob, count={}, element_size={}", count, sizeof(T));
            return VK_ERROR_UNKNOWN;
        }
        return patch_pointer(pointer_slot, pointer_slot_offset, target);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying plain array, count={}, element_size={}, align={}", count,
                        sizeof(T), alignof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult pack_string(const char * value, Blob & blob, std::size_t pointer_slot_offset, const char *& pointer_slot) {
    if (!value) [[unlikely]] { return patch_pointer(pointer_slot, pointer_slot_offset, 0u); }
    try {
        const std::size_t size        = std::strlen(value) + 1;
        std::size_t       target      = 0;
        auto              destination = blob.grow<char>(size, alignof(char), &target);
        if (destination.set(0, size, value) != size) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy string into blob, pointer_slot_offset={}", pointer_slot_offset);
            return VK_ERROR_UNKNOWN;
        }
        return patch_pointer(pointer_slot, pointer_slot_offset, target);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying string, pointer_slot_offset={}", pointer_slot_offset);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult pack_string_array(const char * const * values, std::uint32_t count, Blob & blob, std::size_t pointer_slot_offset, const char * const *& pointer_slot) {
    if (count == 0 || !values) [[unlikely]] { return patch_pointer(pointer_slot, pointer_slot_offset, 0u); }

    try {
        std::size_t          array_offset  = 0;
        auto                 pointer_slots = blob.grow<std::uintptr_t>(count, alignof(std::uintptr_t), &array_offset);
        const std::uintptr_t zero          = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!pointer_slots.set(i, zero)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not initialize string-array slot, array_offset={}, index={}", array_offset, i);
                return VK_ERROR_UNKNOWN;
            }
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            if (!values[i]) [[unlikely]] { continue; }
            const std::size_t string_size   = std::strlen(values[i]) + 1;
            std::size_t       string_offset = 0;
            auto              string_view   = blob.grow<char>(string_size, alignof(char), &string_offset);
            if (string_view.set(0, string_size, values[i]) != string_size) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not copy string-array element, array_offset={}, index={}", array_offset, i);
                return VK_ERROR_UNKNOWN;
            }
            const std::size_t    slot_offset = array_offset + i * sizeof(std::uintptr_t);
            const std::uintptr_t encoded     = static_cast<std::uintptr_t>(string_offset - slot_offset);
            if (!pointer_slots.set(i, encoded)) [[unlikely]] {
                VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: could not patch string-array element, array_offset={}, index={}, string_offset={}",
                                array_offset, i, string_offset);
                return VK_ERROR_UNKNOWN;
            }
        }
        return patch_pointer(pointer_slot, pointer_slot_offset, array_offset);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry structure pack failed: out of host memory while copying string array, count={}, pointer_slot_offset={}", count,
                        pointer_slot_offset);
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
    return patch_pointer(packed_value->pNext, packed.offset + offsetof(T, pNext), pnext.offset);
}

template<class T>
VkResult pack_plain_typed_pnext(const T * value, Blob & blob, PackedStruct & packed) {
    T * packed_value = nullptr;
    return pack_plain_typed_pnext(value, blob, packed, packed_value);
}

template<class T>
VkResult unpack_typed_view(SafeArrayView<std::uint8_t> & view, VkStructureType expected_stype, T ** value) {
    if (!value) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: output pointer for typed view is null, expected_sType={}", static_cast<int>(expected_stype));
        return VK_ERROR_UNKNOWN;
    }
    *value       = nullptr;
    auto * typed = view.size() < sizeof(T) ? nullptr : reinterpret_cast<T *>(view.address(0));
    if (!typed) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: view does not contain typed record, view_size={}, size={}, expected_sType={}", view.size(),
                        sizeof(T), static_cast<int>(expected_stype));
        return VK_ERROR_UNKNOWN;
    }
    if (typed->sType != expected_stype) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: sType mismatch, expected_sType={}, actual_sType={}", static_cast<int>(expected_stype),
                        static_cast<int>(typed->sType));
        return VK_ERROR_UNKNOWN;
    }
    *value = typed;
    return VK_SUCCESS;
}

SafeArrayView<std::uint8_t> tail_view_from_pointer(SafeArrayView<std::uint8_t> & view, const void * pointer) {
    auto * begin = view.address(0);
    if (!begin || !pointer) { return {}; }
    auto * target = const_cast<std::uint8_t *>(reinterpret_cast<const std::uint8_t *>(pointer));
    auto * end    = begin + view.size();
    if (target < begin || target >= end) { return {}; }
    return SafeArrayView<std::uint8_t>(static_cast<std::size_t>(end - target), target);
}

template<class Pointer>
VkResult recover_pointer(Pointer & pointer_slot, SafeArrayView<std::uint8_t> & view) {
    if (!pointer_slot) { return VK_SUCCESS; }
    auto * begin = view.address(0);
    if (!begin) [[unlikely]] { return VK_ERROR_UNKNOWN; }
    auto * slot   = reinterpret_cast<std::uint8_t *>(&pointer_slot);
    auto * end    = begin + view.size();
    auto * target = slot + reinterpret_cast<std::uintptr_t>(pointer_slot);
    if (slot < begin || slot + sizeof(Pointer) > end || target < begin || target >= end) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: encoded pointer is outside view, slot={}, target={}, view_size={}",
                        static_cast<const void *>(slot), static_cast<const void *>(target), view.size());
        return VK_ERROR_UNKNOWN;
    }
    pointer_slot = reinterpret_cast<Pointer>(target);
    return VK_SUCCESS;
}

VkResult recover_string_array(const char * const *& pointer_slot, std::uint32_t count, SafeArrayView<std::uint8_t> & view) {
    if (count == 0 || !pointer_slot) { return VK_SUCCESS; }
    auto * begin = view.address(0);
    if (!begin) [[unlikely]] { return VK_ERROR_UNKNOWN; }
    auto *            slot         = reinterpret_cast<std::uint8_t *>(&pointer_slot);
    auto *            array_target = slot + reinterpret_cast<std::uintptr_t>(pointer_slot);
    auto *            end          = begin + view.size();
    const std::size_t array_size   = count * sizeof(std::uintptr_t);
    if (slot < begin || slot + sizeof(pointer_slot) > end || array_target < begin || array_size > static_cast<std::size_t>(end - array_target)) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: encoded string array is outside view, count={}, view_size={}", count, view.size());
        return VK_ERROR_UNKNOWN;
    }

    auto * slots = reinterpret_cast<std::uintptr_t *>(array_target);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (slots[i] == 0) { continue; }
        auto * element_slot   = reinterpret_cast<std::uint8_t *>(&slots[i]);
        auto * element_target = element_slot + slots[i];
        if (element_target < begin || element_target >= end) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry structure unpack failed: encoded string element is outside view, index={}, view_size={}", i, view.size());
            return VK_ERROR_UNKNOWN;
        }
        slots[i] = reinterpret_cast<std::uintptr_t>(element_target);
    }
    pointer_slot = reinterpret_cast<const char * const *>(array_target);
    return VK_SUCCESS;
}

template<class Pointer>
VkResult recover_pnext_chain(Pointer & pointer_slot, SafeArrayView<std::uint8_t> & view) {
    VkResult status = recover_pointer(pointer_slot, view);
    if (status != VK_SUCCESS || !pointer_slot) { return status; }
    auto pnext_view = tail_view_from_pointer(view, pointer_slot);
    if (pnext_view.empty()) [[unlikely]] { return VK_ERROR_UNKNOWN; }
    const void * ignored = nullptr;
    return unpack_pnext_chain(pnext_view, &ignored);
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
    status = patch_pointer(packed_value->pNext, packed.offset + offsetof(VkApplicationInfo, pNext), pnext.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string(value->pApplicationName, blob, packed.offset + offsetof(VkApplicationInfo, pApplicationName), packed_value->pApplicationName);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_string(value->pEngineName, blob, packed.offset + offsetof(VkApplicationInfo, pEngineName), packed_value->pEngineName);
}

VkResult pack_VkInstanceCreateInfo(const VkInstanceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkInstanceCreateInfo * packed_value = nullptr;
    VkResult               status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, packed.offset + offsetof(VkInstanceCreateInfo, pNext), pnext.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    PackedStruct app;
    status = pack_VkApplicationInfo(value->pApplicationInfo, blob, app);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pApplicationInfo, packed.offset + offsetof(VkInstanceCreateInfo, pApplicationInfo), app.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed.offset + offsetof(VkInstanceCreateInfo, ppEnabledLayerNames),
                               packed_value->ppEnabledLayerNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob,
                             packed.offset + offsetof(VkInstanceCreateInfo, ppEnabledExtensionNames), packed_value->ppEnabledExtensionNames);
}

VkResult pack_VkDeviceQueueCreateInfo(const VkDeviceQueueCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceQueueCreateInfo * packed_value = nullptr;
    VkResult                  status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, packed.offset + offsetof(VkDeviceQueueCreateInfo, pNext), pnext.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_plain_array(value->pQueuePriorities, value->queueCount, blob, packed.offset + offsetof(VkDeviceQueueCreateInfo, pQueuePriorities),
                            packed_value->pQueuePriorities);
}

VkResult pack_VkDeviceCreateInfo(const VkDeviceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceCreateInfo * packed_value = nullptr;
    VkResult             status       = append_shallow_struct(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    PackedStruct pnext;
    status = pack_pnext_chain(value->pNext, blob, pnext);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_pointer(packed_value->pNext, packed.offset + offsetof(VkDeviceCreateInfo, pNext), pnext.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }

    if (value->queueCreateInfoCount == 0 || !value->pQueueCreateInfos) [[unlikely]] {
        status = patch_pointer(packed_value->pQueueCreateInfos, packed.offset + offsetof(VkDeviceCreateInfo, pQueueCreateInfos), 0u);
    } else {
        const std::size_t array_offset = blob.size();
        for (std::uint32_t i = 0; i < value->queueCreateInfoCount; ++i) {
            PackedStruct child;
            status = pack_VkDeviceQueueCreateInfo(&value->pQueueCreateInfos[i], blob, child);
            if (status != VK_SUCCESS) [[unlikely]] { return status; }
        }
        status = patch_pointer(packed_value->pQueueCreateInfos, packed.offset + offsetof(VkDeviceCreateInfo, pQueueCreateInfos), array_offset);
    }
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledLayerNames, value->enabledLayerCount, blob, packed.offset + offsetof(VkDeviceCreateInfo, ppEnabledLayerNames),
                               packed_value->ppEnabledLayerNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_string_array(value->ppEnabledExtensionNames, value->enabledExtensionCount, blob,
                               packed.offset + offsetof(VkDeviceCreateInfo, ppEnabledExtensionNames), packed_value->ppEnabledExtensionNames);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return pack_plain_array(value->pEnabledFeatures, value->pEnabledFeatures ? 1u : 0u, blob, packed.offset + offsetof(VkDeviceCreateInfo, pEnabledFeatures),
                            packed_value->pEnabledFeatures);
}

VkResult pack_VkDeviceGroupDeviceCreateInfo(const VkDeviceGroupDeviceCreateInfo * value, Blob & blob, PackedStruct & packed) {
    VkDeviceGroupDeviceCreateInfo * packed_value = nullptr;
    VkResult                        status       = pack_plain_typed_pnext(value, blob, packed, packed_value);
    if (status != VK_SUCCESS || !value) [[unlikely]] { return status; }
    return pack_plain_array(value->pPhysicalDevices, value->physicalDeviceCount, blob,
                            packed.offset + offsetof(VkDeviceGroupDeviceCreateInfo, pPhysicalDevices), packed_value->pPhysicalDevices);
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

VkResult unpack_VkApplicationInfo(SafeArrayView<std::uint8_t> & view, const VkApplicationInfo ** value) {
    VkApplicationInfo * typed  = nullptr;
    VkResult            status = unpack_typed_view(view, VK_STRUCTURE_TYPE_APPLICATION_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pApplicationName, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pEngineName, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkInstanceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkInstanceCreateInfo ** value) {
    VkInstanceCreateInfo * typed  = nullptr;
    VkResult               status = unpack_typed_view(view, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pApplicationInfo, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    if (typed->pApplicationInfo) {
        auto                      child_view = tail_view_from_pointer(view, typed->pApplicationInfo);
        const VkApplicationInfo * ignored    = nullptr;
        status                               = unpack_VkApplicationInfo(child_view, &ignored);
        if (status != VK_SUCCESS) [[unlikely]] { return status; }
    }
    status = recover_string_array(typed->ppEnabledLayerNames, typed->enabledLayerCount, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_string_array(typed->ppEnabledExtensionNames, typed->enabledExtensionCount, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkDeviceQueueCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceQueueCreateInfo ** value) {
    VkDeviceQueueCreateInfo * typed  = nullptr;
    VkResult                  status = unpack_typed_view(view, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pQueuePriorities, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkDeviceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceCreateInfo ** value) {
    VkDeviceCreateInfo * typed  = nullptr;
    VkResult             status = unpack_typed_view(view, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pQueueCreateInfos, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    for (std::uint32_t i = 0; typed->pQueueCreateInfos && i < typed->queueCreateInfoCount; ++i) {
        auto                            child_view = tail_view_from_pointer(view, &typed->pQueueCreateInfos[i]);
        const VkDeviceQueueCreateInfo * ignored    = nullptr;
        status                                     = unpack_VkDeviceQueueCreateInfo(child_view, &ignored);
        if (status != VK_SUCCESS) [[unlikely]] { return status; }
    }
    status = recover_string_array(typed->ppEnabledLayerNames, typed->enabledLayerCount, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_string_array(typed->ppEnabledExtensionNames, typed->enabledExtensionCount, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pEnabledFeatures, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkDeviceGroupDeviceCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceGroupDeviceCreateInfo ** value) {
    VkDeviceGroupDeviceCreateInfo * typed  = nullptr;
    VkResult                        status = unpack_typed_view(view, VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pointer(typed->pPhysicalDevices, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceFeatures2(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceFeatures2 ** value) {
    VkPhysicalDeviceFeatures2 * typed  = nullptr;
    VkResult                    status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceVulkan11Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan11Features ** value) {
    VkPhysicalDeviceVulkan11Features * typed  = nullptr;
    VkResult                           status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceVulkan12Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan12Features ** value) {
    VkPhysicalDeviceVulkan12Features * typed  = nullptr;
    VkResult                           status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceVulkan13Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan13Features ** value) {
    VkPhysicalDeviceVulkan13Features * typed  = nullptr;
    VkResult                           status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceVulkan14Features(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceVulkan14Features ** value) {
    VkPhysicalDeviceVulkan14Features * typed  = nullptr;
    VkResult                           status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkPhysicalDeviceDescriptorIndexingFeatures(SafeArrayView<std::uint8_t> & view, const VkPhysicalDeviceDescriptorIndexingFeatures ** value) {
    VkPhysicalDeviceDescriptorIndexingFeatures * typed  = nullptr;
    VkResult                                     status = unpack_typed_view(view, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_VkDeviceQueueGlobalPriorityCreateInfo(SafeArrayView<std::uint8_t> & view, const VkDeviceQueueGlobalPriorityCreateInfo ** value) {
    VkDeviceQueueGlobalPriorityCreateInfo * typed  = nullptr;
    VkResult                                status = unpack_typed_view(view, VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO, &typed);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = recover_pnext_chain(typed->pNext, view);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    *value = typed;
    return VK_SUCCESS;
}

VkResult unpack_pnext_chain(SafeArrayView<std::uint8_t> & view, const void ** value) {
    if (!value) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry pNext unpack failed: output pointer is null");
        return VK_ERROR_UNKNOWN;
    }
    *value = nullptr;
    if (view.size() < sizeof(VkStructureType)) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry pNext unpack failed: view does not contain pNext node header, view_size={}", view.size());
        return VK_ERROR_UNKNOWN;
    }

    const auto * base = reinterpret_cast<const VkBaseInStructure *>(view.address(0));
    *value            = base;
    switch (base->sType) {
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO: {
        const VkDeviceGroupDeviceCreateInfo * ignored = nullptr;
        return unpack_VkDeviceGroupDeviceCreateInfo(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2: {
        const VkPhysicalDeviceFeatures2 * ignored = nullptr;
        return unpack_VkPhysicalDeviceFeatures2(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
        const VkPhysicalDeviceVulkan11Features * ignored = nullptr;
        return unpack_VkPhysicalDeviceVulkan11Features(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
        const VkPhysicalDeviceVulkan12Features * ignored = nullptr;
        return unpack_VkPhysicalDeviceVulkan12Features(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
        const VkPhysicalDeviceVulkan13Features * ignored = nullptr;
        return unpack_VkPhysicalDeviceVulkan13Features(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES: {
        const VkPhysicalDeviceVulkan14Features * ignored = nullptr;
        return unpack_VkPhysicalDeviceVulkan14Features(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
        const VkPhysicalDeviceDescriptorIndexingFeatures * ignored = nullptr;
        return unpack_VkPhysicalDeviceDescriptorIndexingFeatures(view, &ignored);
    }
    case VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO: {
        const VkDeviceQueueGlobalPriorityCreateInfo * ignored = nullptr;
        return unpack_VkDeviceQueueGlobalPriorityCreateInfo(view, &ignored);
    }
    default:
        VKFWD_LOG_ERROR("vkfwd ferry pNext unpack failed: unsupported sType={}", static_cast<int>(base->sType));
        return VK_ERROR_UNKNOWN;
    }
}

} // namespace vkfwd::generated::structure
