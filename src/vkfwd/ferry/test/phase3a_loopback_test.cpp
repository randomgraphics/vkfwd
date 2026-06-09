#include "../sample/loopback_runtime.hpp"
#include "forwarder.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::test {
namespace {

// Phase 3a C2.1 mirror of phase2_loopback_test, but for the COHERENT path.
// Same allocation/atom sizes for symmetry with the rest of the loopback tests;
// the load-bearing invariant locked in here is that bytes flow both directions
// AT MAP/UNMAP TIME (not flush/invalidate) on a coherent allocation.
constexpr std::size_t  kAllocationSize        = 8192;
constexpr VkDeviceSize kNonCoherentAtomSize   = 64;
constexpr std::size_t  kMinMemoryMapAlignment = 4096;

// Receiver-process backing store for vkMapMemory. Pre-fill BEFORE map() so the
// receiver→source bracketed copy lands the sentinel in source staging, then
// observe the post-unmap state to confirm the source→receiver bracketed copy
// also fired.
alignas(64) std::uint8_t g_receiver_backing_store[kAllocationSize];

// Same indirection pattern as the Phase 1/2 tests: fake PFNs cannot capture
// state so each TEST_CASE installs a pointer the file-scope fakes read from.
struct Scenario {
    VkInstance       instance        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkDeviceMemory   memory          = VK_NULL_HANDLE;

    bool saw_create_instance                       = false;
    bool saw_enumerate_physical_devices            = false;
    bool saw_get_physical_device_properties        = false;
    bool saw_get_physical_device_memory_properties = false;
    bool saw_create_device                         = false;
    bool saw_allocate_memory                       = false;
    bool saw_map_memory                            = false;
    bool saw_unmap_memory                          = false;

    // Locked-in expectations: the receiver should call vkMapMemory with the
    // exact extent the source requested (after VK_WHOLE_SIZE resolution), and
    // never call vkFlush/InvalidateMappedMemoryRanges on a coherent allocation.
    VkDeviceSize map_offset       = 0;
    VkDeviceSize map_size         = 0;
    bool         saw_flush_call   = false; // must remain false; coherent never flushes
    bool         saw_invalid_call = false; // must remain false; coherent never invalidates
};
Scenario * g_current_scenario = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateInstance(const VkInstanceCreateInfo * pCreateInfo, const VkAllocationCallbacks *, VkInstance * pInstance) {
    REQUIRE(g_current_scenario != nullptr);
    REQUIRE(pCreateInfo != nullptr);
    REQUIRE(pInstance != nullptr);
    *pInstance                              = g_current_scenario->instance;
    g_current_scenario->saw_create_instance = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyInstance(VkInstance, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkEnumeratePhysicalDevices(VkInstance instance, std::uint32_t * pCount, VkPhysicalDevice * pHandles) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(instance == g_current_scenario->instance);
    REQUIRE(pCount != nullptr);
    g_current_scenario->saw_enumerate_physical_devices = true;
    if (!pHandles) {
        *pCount = 1;
        return VK_SUCCESS;
    }
    if (*pCount < 1) { return VK_INCOMPLETE; }
    pHandles[0] = g_current_scenario->physical_device;
    *pCount     = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties * pProperties) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(physicalDevice == g_current_scenario->physical_device);
    REQUIRE(pProperties != nullptr);
    g_current_scenario->saw_get_physical_device_properties = true;
    *pProperties                                           = VkPhysicalDeviceProperties {};
    pProperties->limits.nonCoherentAtomSize                = kNonCoherentAtomSize;
    pProperties->limits.minMemoryMapAlignment              = kMinMemoryMapAlignment;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties * pMemoryProperties) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(physicalDevice == g_current_scenario->physical_device);
    REQUIRE(pMemoryProperties != nullptr);
    g_current_scenario->saw_get_physical_device_memory_properties = true;
    *pMemoryProperties                                            = VkPhysicalDeviceMemoryProperties {};
    pMemoryProperties->memoryTypeCount                            = 1;
    // Coherent host-visible memory type. The forwarder's allocate hook reads
    // this exact propertyFlags through MemoryTypeRegistry and the factory
    // picks CoherentForwarderAllocation rather than the N2 subclass — the
    // whole point of this test is that the C2.1 path runs.
    pMemoryProperties->memoryTypes[0]       = VkMemoryType {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    pMemoryProperties->memoryHeapCount      = 1;
    pMemoryProperties->memoryHeaps[0].size  = 64 * 1024 * 1024;
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo * pCreateInfo, const VkAllocationCallbacks *,
                                                   VkDevice * pDevice) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(physicalDevice == g_current_scenario->physical_device);
    REQUIRE(pCreateInfo != nullptr);
    REQUIRE(pDevice != nullptr);
    *pDevice                              = g_current_scenario->device;
    g_current_scenario->saw_create_device = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyDevice(VkDevice, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo * pAllocateInfo, const VkAllocationCallbacks *,
                                                     VkDeviceMemory * pMemory) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    REQUIRE(pAllocateInfo != nullptr);
    CHECK(pAllocateInfo->memoryTypeIndex == 0);
    CHECK(pAllocateInfo->allocationSize == kAllocationSize);
    REQUIRE(pMemory != nullptr);
    *pMemory                                = g_current_scenario->memory;
    g_current_scenario->saw_allocate_memory = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags,
                                                void ** ppData) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    CHECK(memory == g_current_scenario->memory);
    REQUIRE(ppData != nullptr);
    g_current_scenario->map_offset     = offset;
    g_current_scenario->map_size       = size;
    *ppData                            = g_receiver_backing_store + offset;
    g_current_scenario->saw_map_memory = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    CHECK(memory == g_current_scenario->memory);
    g_current_scenario->saw_unmap_memory = true;
}

// Coherent C2.1 must NEVER call these on the receiver side — the forwarder
// rejects flush/invalidate with VK_ERROR_FEATURE_NOT_PRESENT before any chunk
// reaches the wire. Recording a call here would mean the wire-format guard
// was bypassed.
VKAPI_ATTR VkResult VKAPI_CALL fake_vkFlushMappedMemoryRanges(VkDevice, std::uint32_t, const VkMappedMemoryRange *) {
    REQUIRE(g_current_scenario != nullptr);
    g_current_scenario->saw_flush_call = true;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL fake_vkInvalidateMappedMemoryRanges(VkDevice, std::uint32_t, const VkMappedMemoryRange *) {
    REQUIRE(g_current_scenario != nullptr);
    g_current_scenario->saw_invalid_call = true;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetDeviceProcAddr(VkDevice, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkDestroyDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyDevice); }
    if (std::strcmp(name, "vkAllocateMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkAllocateMemory); }
    if (std::strcmp(name, "vkFreeMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkFreeMemory); }
    if (std::strcmp(name, "vkMapMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkMapMemory); }
    if (std::strcmp(name, "vkUnmapMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkUnmapMemory); }
    if (std::strcmp(name, "vkFlushMappedMemoryRanges") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkFlushMappedMemoryRanges); }
    if (std::strcmp(name, "vkInvalidateMappedMemoryRanges") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkInvalidateMappedMemoryRanges); }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetInstanceProcAddr(VkInstance, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkCreateInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateInstance); }
    if (std::strcmp(name, "vkDestroyInstance") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyInstance); }
    if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkEnumeratePhysicalDevices); }
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetPhysicalDeviceProperties); }
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetPhysicalDeviceMemoryProperties);
    }
    if (std::strcmp(name, "vkCreateDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkCreateDevice); }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkGetDeviceProcAddr); }
    return nullptr;
}

void prepare_scenario(Scenario & scenario, std::uint8_t receiver_fill_byte) {
    std::memset(g_receiver_backing_store, receiver_fill_byte, sizeof(g_receiver_backing_store));
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    registry.forget_device(scenario.device);
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(scenario.memory);
    g_current_scenario = &scenario;
}

void teardown_scenario(Scenario & scenario) {
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    registry.forget_device(scenario.device);
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(scenario.memory);
    g_current_scenario = nullptr;
}

} // namespace

TEST_CASE("phase3a loopback: coherent map fetches receiver bytes; unmap pushes source bytes", "[loopback]") {
    // The single load-bearing Phase 3a test: a COHERENT host-visible
    // allocation goes through the full forwarder→wire→receiver round-trip
    // and BOTH directions of the C2.1 bracketed copy must fire.
    //
    // - On map: receiver-side pre-fill (0x77) MUST appear in source staging
    //   immediately after vkMapMemory returns. The Phase 1/2 N2 tests lock
    //   the OPPOSITE invariant (no transfer on map), so a regression that
    //   short-circuits the C2.1 payload would fail here AND pass there.
    //
    // - On unmap: source-side write (0x99) MUST appear in the receiver
    //   backing store immediately after vkUnmapMemory returns. Again, the
    //   N2 test locks the opposite invariant for non-coherent unmap.
    //
    // - flush/invalidate PFNs MUST NEVER be called for a coherent
    //   allocation — the forwarder rejects with VK_ERROR_FEATURE_NOT_PRESENT
    //   before reaching the wire; the C2.1 unmap chunk carries the bytes
    //   directly. saw_flush_call / saw_invalid_call lock that in.
    Scenario scenario {
        .instance        = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xC0FFEE31)),
        .physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xC0FFEE32)),
        .device          = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xC0FFEE33)),
        .memory          = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(0xC0FFEE34)),
    };
    constexpr std::uint8_t kReceiverPreMapByte = 0x77;
    constexpr std::uint8_t kSourceWriteByte    = 0x99;
    prepare_scenario(scenario, kReceiverPreMapByte);
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    // Standup matches the Phase 2 prefix; coherent vs non-coherent classification
    // happens via the memory-type propertyFlags reported above, not via the
    // standup sequence.
    VkInstanceCreateInfo instance_ci {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance           instance = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateInstance_entry(&instance_ci, nullptr, &instance) == VK_SUCCESS);

    std::uint32_t physical_device_count = 0;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, nullptr) == VK_SUCCESS);
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, &physical_device) == VK_SUCCESS);

    VkPhysicalDeviceProperties properties {};
    forwarder::generated::vkGetPhysicalDeviceProperties_entry(physical_device, &properties);
    VkPhysicalDeviceMemoryProperties memory_properties {};
    forwarder::generated::vkGetPhysicalDeviceMemoryProperties_entry(physical_device, &memory_properties);

    // Sanity: confirm the memory type is the coherent host-visible one. If
    // this assertion ever fails, the rest of the test would silently exercise
    // the N2 path and pass for the wrong reason.
    REQUIRE(memory_properties.memoryTypeCount == 1);
    REQUIRE((memory_properties.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);
    REQUIRE((memory_properties.memoryTypes[0].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0);

    VkDeviceCreateInfo device_ci {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice           device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateDevice_entry(physical_device, &device_ci, nullptr, &device) == VK_SUCCESS);

    VkMemoryAllocateInfo alloc_info {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = kAllocationSize,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkAllocateMemory_entry(device, &alloc_info, nullptr, &memory) == VK_SUCCESS);
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == kAllocationSize);

    // Step 1: map. Goes through CoherentForwarderAllocation::map →
    // MemoryMap wire chunk → CoherentReceiverAllocation::map_endpoint →
    // fake_vkMapMemory → response stream carrying effective_size payload
    // bytes from g_receiver_backing_store → forwarder copies into source
    // staging at offset 0.
    void * ppData = nullptr;
    REQUIRE(forwarder::generated::vkMapMemory_entry(device, memory, 0, VK_WHOLE_SIZE, 0, &ppData) == VK_SUCCESS);
    CHECK(scenario.saw_map_memory);
    REQUIRE(ppData != nullptr);
    // Receiver-process address must NOT leak — the staging pointer the source
    // sees is the forwarder's vm::reserve'd VA, not the receiver backing store.
    CHECK(ppData != static_cast<void *>(g_receiver_backing_store));

    // Step 2 (C2.1 receiver→source invariant): source staging must now
    // mirror the receiver's pre-map fill. This is the WHOLE POINT of the
    // C2.1 strategy on the map side — a GPU compute output written into
    // coherent memory becomes visible to a CPU read without an explicit
    // invalidate, which the spec says would be a no-op anyway.
    {
        auto * const source_bytes = static_cast<const std::uint8_t *>(ppData);
        for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(source_bytes[i] == kReceiverPreMapByte); }
    }

    // Step 3: write a different sentinel through the source pointer. The
    // C2.1 unmap chunk must carry these bytes to the receiver.
    std::memset(ppData, kSourceWriteByte, kAllocationSize);

    // Receiver backing store should still hold the pre-map fill — coherent
    // memory's spec says host writes are visible to the device, but in this
    // loopback fake there is no real driver that propagates host writes
    // through the mapped pointer; only the C2.1 unmap chunk will move them.
    // This is a locked-in invariant against an accidental "always transfer"
    // bug somewhere in the wire layer.
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(g_receiver_backing_store[i] == kReceiverPreMapByte); }

    // Step 4: unmap. Sends a MemoryUnmap chunk with range_count=1 and
    // mapped_size bytes of payload; receiver writes them into the still-live
    // mapped pointer (which aliases g_receiver_backing_store) BEFORE calling
    // fake_vkUnmapMemory.
    forwarder::generated::vkUnmapMemory_entry(device, memory);
    CHECK(scenario.saw_unmap_memory);

    // Step 5 (C2.1 source→receiver invariant): the receiver backing store
    // must now reflect the source's sentinel. This is the other half of
    // C2.1 — host writes between map and unmap reach the device without an
    // explicit flush.
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(g_receiver_backing_store[i] == kSourceWriteByte); }

    // Step 6 (negative invariant): flush/invalidate PFNs must never have
    // been called. The forwarder rejects coherent flush/invalidate with
    // VK_ERROR_FEATURE_NOT_PRESENT before the wire; the C2.1 bracketed
    // copies are what move the bytes, NOT a vkFlush/InvalidateMappedMemoryRanges
    // dispatched to the driver.
    CHECK_FALSE(scenario.saw_flush_call);
    CHECK_FALSE(scenario.saw_invalid_call);

    // And the receiver should have driven the local vkMapMemory with the
    // resolved (concrete, non-VK_WHOLE_SIZE) extent — the forwarder
    // resolves VK_WHOLE_SIZE before sending so the receiver sees a number.
    CHECK(scenario.map_offset == 0);
    CHECK(scenario.map_size == kAllocationSize);

    forwarder::generated::vkFreeMemory_entry(device, memory, nullptr);
    forwarder::generated::vkDestroyDevice_entry(device, nullptr);
    forwarder::generated::vkDestroyInstance_entry(instance, nullptr);

    teardown_scenario(scenario);
}

} // namespace vkfwd::test
