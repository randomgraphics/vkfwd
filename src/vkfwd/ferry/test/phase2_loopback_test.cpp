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

// Same limits as the Phase 1 test so atom-alignment math reduces to identity
// for these tests (kAllocationSize is a multiple of kNonCoherentAtomSize). A
// Phase-2-specific test for the non-aligned path is deferred to N4; the load-
// bearing invariant locked in here is "bytes flow both ways across the wire".
constexpr std::size_t  kAllocationSize        = 8192;
constexpr VkDeviceSize kNonCoherentAtomSize   = 64;
constexpr std::size_t  kMinMemoryMapAlignment = 4096;

// Receiver-process backing store for vkMapMemory. The Phase 2 invariant is
// opposite to Phase 1's: after vkFlushMappedMemoryRanges the source byte
// pattern MUST appear here, and after vkInvalidateMappedMemoryRanges the
// pre-seeded receiver byte pattern MUST appear in source staging.
alignas(64) std::uint8_t g_receiver_backing_store[kAllocationSize];

// File-scope scenario pointer — same indirection pattern as phase1_loopback_test.cpp:
// fake PFNs cannot capture state, so each TEST_CASE installs a current-scenario
// pointer and the fakes read it. Each TEST_CASE mints unique handles to dodge
// the file-scope MemoryTypeRegistry / MemoryMapForwarder singletons that
// persist across the binary.
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
    bool saw_flush                                 = false;
    bool saw_invalidate                            = false;

    // Locked-in expectations for Step 6 of each test. Captured here so the
    // fake PFN can assert exactly what the receiver passed it, not just that
    // it was called at all.
    VkDeviceSize flush_offset      = 0;
    VkDeviceSize flush_size        = 0;
    VkDeviceSize invalidate_offset = 0;
    VkDeviceSize invalidate_size   = 0;
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
    pMemoryProperties->memoryTypes[0]                             = VkMemoryType {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    pMemoryProperties->memoryHeapCount                            = 1;
    pMemoryProperties->memoryHeaps[0].size                        = 64 * 1024 * 1024;
    pMemoryProperties->memoryHeaps[0].flags                       = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
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

VKAPI_ATTR VkResult VKAPI_CALL fake_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize, VkMemoryMapFlags, void ** ppData) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    CHECK(memory == g_current_scenario->memory);
    REQUIRE(ppData != nullptr);
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

VKAPI_ATTR VkResult VKAPI_CALL fake_vkFlushMappedMemoryRanges(VkDevice device, std::uint32_t memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    REQUIRE(memoryRangeCount == 1);
    REQUIRE(pMemoryRanges != nullptr);
    CHECK(pMemoryRanges[0].sType == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    CHECK(pMemoryRanges[0].memory == g_current_scenario->memory);
    g_current_scenario->saw_flush    = true;
    g_current_scenario->flush_offset = pMemoryRanges[0].offset;
    g_current_scenario->flush_size   = pMemoryRanges[0].size;
    return VK_SUCCESS;
}

// Invalidate fills the receiver backing store BEFORE returning so the receiver
// invalidate_endpoint reads the post-driver bytes when it copies back to the
// response stream. This models a real driver that pulls from device memory
// into the host-visible mapping in response to vkInvalidateMappedMemoryRanges.
VkDeviceSize                   g_invalidate_seed_offset  = 0;
VkDeviceSize                   g_invalidate_seed_size    = 0;
std::uint8_t                   g_invalidate_seed_pattern = 0;
VKAPI_ATTR VkResult VKAPI_CALL fake_vkInvalidateMappedMemoryRanges(VkDevice device, std::uint32_t memoryRangeCount, const VkMappedMemoryRange * pMemoryRanges) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    REQUIRE(memoryRangeCount == 1);
    REQUIRE(pMemoryRanges != nullptr);
    CHECK(pMemoryRanges[0].sType == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    CHECK(pMemoryRanges[0].memory == g_current_scenario->memory);
    g_current_scenario->saw_invalidate    = true;
    g_current_scenario->invalidate_offset = pMemoryRanges[0].offset;
    g_current_scenario->invalidate_size   = pMemoryRanges[0].size;
    // Simulate a driver pulling fresh device-memory bytes into the host-visible
    // mapping. The receiver invalidate_endpoint memcpys these bytes back into
    // the response stream after we return.
    if (g_invalidate_seed_size != 0) {
        std::memset(g_receiver_backing_store + g_invalidate_seed_offset, g_invalidate_seed_pattern, static_cast<std::size_t>(g_invalidate_seed_size));
    }
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

// Mirrors phase1_loopback_test's prepare/teardown: each TEST_CASE mints
// unique handles so the file-scope singletons cannot leak state between cases.
void prepare_scenario(Scenario & scenario, std::uint8_t receiver_fill_byte) {
    std::memset(g_receiver_backing_store, receiver_fill_byte, sizeof(g_receiver_backing_store));
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    registry.forget_device(scenario.device);
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(scenario.memory);
    g_current_scenario        = &scenario;
    g_invalidate_seed_offset  = 0;
    g_invalidate_seed_size    = 0;
    g_invalidate_seed_pattern = 0;
}

void teardown_scenario(Scenario & scenario) {
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    registry.forget_device(scenario.device);
    ::vkfwd::MemoryMapForwarder::instance().forget_allocation(scenario.memory);
    g_current_scenario = nullptr;
}

// Walks the standard instance/device/allocate path up to (and including) the
// vkMapMemory call, returning the source staging pointer. Both TEST_CASEs need
// this prefix; pulling it out keeps each TEST_CASE focused on the Phase 2
// transfer-direction invariant it actually exercises.
void * standup_and_map(Scenario & scenario) {
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
    REQUIRE(memory == scenario.memory);

    void * ppData = nullptr;
    REQUIRE(forwarder::generated::vkMapMemory_entry(device, memory, 0, VK_WHOLE_SIZE, 0, &ppData) == VK_SUCCESS);
    REQUIRE(ppData != nullptr);
    return ppData;
}

} // namespace

TEST_CASE("phase2 loopback: vkFlushMappedMemoryRanges propagates source bytes to receiver memory", "[loopback]") {
    // Phase 2 invariant locked in here: with flush wired, source writes through
    // the staging pointer reach the receiver backing store. The Phase 1 test
    // locked the OPPOSITE invariant (no transfer without flush); this is the
    // counterpart that proves the wire really moves bytes.
    Scenario scenario {
        .instance        = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xF1A570B1)),
        .physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xF1A570B2)),
        .device          = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xF1A570B3)),
        .memory          = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(0xF1A570B4)),
    };
    constexpr std::uint8_t kPreFlushReceiverByte = 0xAA;
    constexpr std::uint8_t kSourceWriteByte      = 0x55;
    prepare_scenario(scenario, kPreFlushReceiverByte);
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    void * const staging = standup_and_map(scenario);

    // Step 1: write a sentinel through source staging.
    std::memset(staging, kSourceWriteByte, kAllocationSize);

    // Step 2 (load-bearing): before flush, the receiver backing store must
    // still be untouched. This locks Phase 1's no-transfer-without-flush
    // invariant into the Phase 2 test too, so a regression that always
    // transferred bytes (e.g. on map or unmap) would fail here.
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(g_receiver_backing_store[i] == kPreFlushReceiverByte); }

    // Step 3: flush. Drives the full forwarder→wire→receiver round-trip plus
    // the receiver's real vkFlushMappedMemoryRanges PFN.
    VkMappedMemoryRange range {
        .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .pNext  = nullptr,
        .memory = scenario.memory,
        .offset = 0,
        .size   = VK_WHOLE_SIZE,
    };
    REQUIRE(forwarder::generated::vkFlushMappedMemoryRanges_entry(scenario.device, 1, &range) == VK_SUCCESS);
    CHECK(scenario.saw_flush);
    // The receiver atom-aligns the source-supplied offset/size; here the
    // mapped extent is already atom-aligned so we expect the exact bounds.
    CHECK(scenario.flush_offset == 0);
    CHECK(scenario.flush_size == kAllocationSize);

    // Step 4: receiver backing store must now mirror the source pattern.
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(g_receiver_backing_store[i] == kSourceWriteByte); }

    forwarder::generated::vkUnmapMemory_entry(scenario.device, scenario.memory);
    teardown_scenario(scenario);
}

TEST_CASE("phase2 loopback: vkInvalidateMappedMemoryRanges propagates receiver bytes to source", "[loopback]") {
    // Mirror of the flush test for the opposite direction. The fake invalidate
    // PFN paints the receiver backing store after the driver call returns
    // (modeling a real driver flushing device memory into the mapping); the
    // receiver invalidate_endpoint then ships those bytes back to source.
    Scenario scenario {
        .instance        = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xF1A570C1)),
        .physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xF1A570C2)),
        .device          = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xF1A570C3)),
        .memory          = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(0xF1A570C4)),
    };
    constexpr std::uint8_t kInitialSourceByte  = 0x33;
    constexpr std::uint8_t kReceiverDriverByte = 0xCC;
    prepare_scenario(scenario, 0x00);
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    void * const staging = standup_and_map(scenario);

    // Step 1: paint source staging with a fingerprint pattern that has to
    // change after invalidate. A success that left bytes unchanged would
    // sneak past a simple "is the receiver byte present" check.
    std::memset(staging, kInitialSourceByte, kAllocationSize);

    // Step 2: arm the fake driver — when it sees the real
    // vkInvalidateMappedMemoryRanges call, it will paint the receiver mapping
    // before returning so the receiver endpoint then reads kReceiverDriverByte.
    g_invalidate_seed_offset  = 0;
    g_invalidate_seed_size    = kAllocationSize;
    g_invalidate_seed_pattern = kReceiverDriverByte;

    VkMappedMemoryRange range {
        .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .pNext  = nullptr,
        .memory = scenario.memory,
        .offset = 0,
        .size   = VK_WHOLE_SIZE,
    };
    REQUIRE(forwarder::generated::vkInvalidateMappedMemoryRanges_entry(scenario.device, 1, &range) == VK_SUCCESS);
    CHECK(scenario.saw_invalidate);
    CHECK(scenario.invalidate_offset == 0);
    CHECK(scenario.invalidate_size == kAllocationSize);

    // Step 3: source staging must now carry the receiver-driver byte pattern.
    auto * const bytes = static_cast<const std::uint8_t *>(staging);
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(bytes[i] == kReceiverDriverByte); }

    forwarder::generated::vkUnmapMemory_entry(scenario.device, scenario.memory);
    teardown_scenario(scenario);
}

} // namespace vkfwd::test
