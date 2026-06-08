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

// Common limits used by both scenarios. 4096 is the common Vulkan
// minMemoryMapAlignment floor; 64 is the most common nonCoherentAtomSize.
constexpr std::size_t  kAllocationSize        = 8192;
constexpr VkDeviceSize kNonCoherentAtomSize   = 64;
constexpr std::size_t  kMinMemoryMapAlignment = 4096;
constexpr std::uint8_t kReceiverFillByte      = 0xAA; // pre-fill of receiver memory; locked-in invariant
constexpr std::uint8_t kSourceWriteByte       = 0x55; // pattern source writes through its staging pointer

// Receiver-process backing for vkMapMemory. The Phase 1 N2 invariant locks in
// that source-side writes to the staging pointer do NOT propagate here (no
// flush is wired yet). Aligned to nonCoherentAtomSize so a future flush
// implementation can use it without re-allocation.
alignas(64) std::uint8_t g_receiver_backing_store[kAllocationSize];

// Per-TEST_CASE Scenario lets each test mint UNIQUE Vulkan handles. This is
// load-bearing because the forwarder's MemoryTypeRegistry and ::vkfwd::
// MemoryMapForwarder are file-scope singletons that persist across the entire
// test binary — and the registry has no public "forget physical_device" surface
// for tests to scrub. Two TEST_CASEs that share a VkPhysicalDevice handle
// would leak the first test's recorded memory properties into the second,
// hiding the registry-miss the second test is supposed to trigger.
struct Scenario {
    VkInstance       instance        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice         device          = VK_NULL_HANDLE;
    VkDeviceMemory   memory          = VK_NULL_HANDLE;

    // PFN observability. The forwarder→receiver round-trip is the only path
    // that can flip these, so each flag is direct evidence the corresponding
    // command crossed the wire and reached the receiver-side fake driver.
    bool saw_create_instance                       = false;
    bool saw_destroy_instance                      = false;
    bool saw_enumerate_physical_devices            = false;
    bool saw_get_physical_device_properties        = false;
    bool saw_get_physical_device_memory_properties = false;
    bool saw_create_device                         = false;
    bool saw_destroy_device                        = false;
    bool saw_allocate_memory                       = false;
    bool saw_free_memory                           = false;
    bool saw_map_memory                            = false;
    bool saw_unmap_memory                          = false;
};

// Pointer-to-current-scenario lets the file-scope fake PFNs read which
// handles they should mint/check. The fakes are looked up by name and have
// to be plain function pointers, so they cannot capture state through
// closures; a current-scenario pointer is the workaround.
Scenario * g_current_scenario = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL fake_vkCreateInstance(const VkInstanceCreateInfo * pCreateInfo, const VkAllocationCallbacks *, VkInstance * pInstance) {
    REQUIRE(g_current_scenario != nullptr);
    REQUIRE(pCreateInfo != nullptr);
    REQUIRE(pInstance != nullptr);
    *pInstance                              = g_current_scenario->instance;
    g_current_scenario->saw_create_instance = true;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(instance == g_current_scenario->instance);
    g_current_scenario->saw_destroy_instance = true;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkEnumeratePhysicalDevices(VkInstance instance, std::uint32_t * pCount, VkPhysicalDevice * pHandles) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(instance == g_current_scenario->instance);
    REQUIRE(pCount != nullptr);
    g_current_scenario->saw_enumerate_physical_devices = true;
    // Two-call pattern: pHandles==nullptr returns just the count; second call
    // returns the handles. Both legs must work because vkfwd may forward both
    // (rapid-vulkan and other apps do the count-then-fetch dance).
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
    // Only the limits the forwarder consults — the rest can stay zero.
    *pProperties                              = VkPhysicalDeviceProperties {};
    pProperties->limits.nonCoherentAtomSize   = kNonCoherentAtomSize;
    pProperties->limits.minMemoryMapAlignment = kMinMemoryMapAlignment;
}

VKAPI_ATTR void VKAPI_CALL fake_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties * pMemoryProperties) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(physicalDevice == g_current_scenario->physical_device);
    REQUIRE(pMemoryProperties != nullptr);
    g_current_scenario->saw_get_physical_device_memory_properties = true;
    // Single non-coherent host-visible type. Phase 1 exercises only the
    // non-coherent path; the coherent flag is intentionally absent so the
    // forwarder picks NonCoherentForwarderAllocation.
    *pMemoryProperties                      = VkPhysicalDeviceMemoryProperties {};
    pMemoryProperties->memoryTypeCount      = 1;
    pMemoryProperties->memoryTypes[0]       = VkMemoryType {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
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

VKAPI_ATTR void VKAPI_CALL fake_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    g_current_scenario->saw_destroy_device = true;
}

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

VKAPI_ATTR void VKAPI_CALL fake_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    CHECK(memory == g_current_scenario->memory);
    g_current_scenario->saw_free_memory = true;
}

VKAPI_ATTR VkResult VKAPI_CALL fake_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags,
                                                void ** ppData) {
    REQUIRE(g_current_scenario != nullptr);
    CHECK(device == g_current_scenario->device);
    CHECK(memory == g_current_scenario->memory);
    CHECK(offset == 0);
    // VK_WHOLE_SIZE was resolved by the receiver before reaching here; the
    // value we get should be the recorded allocation size.
    CHECK(size == kAllocationSize);
    CHECK(flags == 0);
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

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_vkGetDeviceProcAddr(VkDevice, const char * name) {
    if (!name) { return nullptr; }
    if (std::strcmp(name, "vkDestroyDevice") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkDestroyDevice); }
    if (std::strcmp(name, "vkAllocateMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkAllocateMemory); }
    if (std::strcmp(name, "vkFreeMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkFreeMemory); }
    if (std::strcmp(name, "vkMapMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkMapMemory); }
    if (std::strcmp(name, "vkUnmapMemory") == 0) { return reinterpret_cast<PFN_vkVoidFunction>(fake_vkUnmapMemory); }
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

// Resets per-test state: receiver backing store, the forwarder's
// MemoryMapForwarder record for THIS scenario's memory handle, and the
// MemoryTypeRegistry's device row (the registry has no forget-physical-device
// surface; each scenario uses a unique VkPhysicalDevice to side-step that).
void prepare_scenario(Scenario & scenario) {
    std::memset(g_receiver_backing_store, kReceiverFillByte, sizeof(g_receiver_backing_store));
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

TEST_CASE("phase1 loopback: non-coherent map+unmap preserves the N2 no-transfer invariant", "[loopback]") {
    // This is the load-bearing Phase 1 test: it drives the full
    // forwarder→transport→receiver path against fake PFNs, exercises the
    // manual MemoryMap/MemoryUnmap chunks end-to-end, and locks in the
    // contract that source writes do NOT propagate to receiver memory until
    // a flush is wired (Phase 2).
    Scenario scenario {
        .instance        = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xC0FFEE01)),
        .physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xC0FFEE02)),
        .device          = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xC0FFEE03)),
        .memory          = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(0xC0FFEE04)),
    };
    prepare_scenario(scenario);
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    // Step 1: instance.
    VkInstanceCreateInfo instance_ci {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance           instance = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateInstance_entry(&instance_ci, nullptr, &instance) == VK_SUCCESS);
    REQUIRE(instance == scenario.instance);
    CHECK(scenario.saw_create_instance);

    // Step 2: enumerate physical devices. The receiver hook records (h, h)
    // into source_to_receiver_physical_device which the QueryPhysicalDeviceMemoryInfo
    // path would need; this test primes via explicit property queries below,
    // so the enumeration here mostly satisfies the realistic Vulkan flow.
    std::uint32_t physical_device_count = 0;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, nullptr) == VK_SUCCESS);
    REQUIRE(physical_device_count == 1);
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, &physical_device) == VK_SUCCESS);
    REQUIRE(physical_device == scenario.physical_device);
    CHECK(scenario.saw_enumerate_physical_devices);

    // Step 3 & 4: query physical-device props and memory props so the
    // forwarder's MemoryTypeRegistry resolves at allocate time without the
    // QueryPhysicalDeviceMemoryInfo fallback. The forwarder's after_response_unpack
    // hooks on each of these commands feed the registry.
    VkPhysicalDeviceProperties properties {};
    forwarder::generated::vkGetPhysicalDeviceProperties_entry(physical_device, &properties);
    CHECK(scenario.saw_get_physical_device_properties);
    CHECK(properties.limits.nonCoherentAtomSize == kNonCoherentAtomSize);
    CHECK(properties.limits.minMemoryMapAlignment == kMinMemoryMapAlignment);

    VkPhysicalDeviceMemoryProperties memory_properties {};
    forwarder::generated::vkGetPhysicalDeviceMemoryProperties_entry(physical_device, &memory_properties);
    CHECK(scenario.saw_get_physical_device_memory_properties);
    CHECK(memory_properties.memoryTypeCount == 1);

    // Step 5: device. The CreateDevice forwarder hook records the
    // device→physical link in the registry so resolve() can find the
    // physical-device row populated in steps 3+4.
    VkDeviceCreateInfo device_ci {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice           device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateDevice_entry(physical_device, &device_ci, nullptr, &device) == VK_SUCCESS);
    REQUIRE(device == scenario.device);
    CHECK(scenario.saw_create_device);

    // Step 6: allocate. Since the registry is primed by steps 3+4, the
    // forwarder hook records the allocation locally — no fallback fires.
    VkMemoryAllocateInfo alloc_info {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = kAllocationSize,
        .memoryTypeIndex = 0,
    };
    VkDeviceMemory memory = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkAllocateMemory_entry(device, &alloc_info, nullptr, &memory) == VK_SUCCESS);
    REQUIRE(memory == scenario.memory);
    CHECK(scenario.saw_allocate_memory);
    // Manager bookkeeping is the proof that the forwarder hook resolved
    // through the registry rather than the fallback path.
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == kAllocationSize);

    // Step 7: map. This goes through MemoryMapForwarder::custom_vkMapMemory_entry
    // → NonCoherentForwarderAllocation::map → manual MemoryMap chunk on the
    // wire → ReceiverSession's manual dispatch → MemoryMapReceiver →
    // NonCoherentReceiverAllocation::map_endpoint → fake_vkMapMemory.
    void * ppData = nullptr;
    REQUIRE(forwarder::generated::vkMapMemory_entry(device, memory, 0, VK_WHOLE_SIZE, 0, &ppData) == VK_SUCCESS);
    CHECK(scenario.saw_map_memory);

    // Step 8: cross-process invariant. *ppData is the SOURCE-side staging
    // pointer. It must be non-null AND it must NEVER be the receiver-process
    // backing-store address — that would mean a receiver pointer escaped to
    // the source app, which is the entire bug class vkfwd's staging exists
    // to prevent.
    REQUIRE(ppData != nullptr);
    CHECK(ppData != static_cast<void *>(g_receiver_backing_store));
    // Source pointer must satisfy Vulkan's minMemoryMapAlignment contract:
    // (mapped - offset) is a multiple of minMemoryMapAlignment. offset is 0
    // here so the pointer itself must be aligned.
    CHECK(reinterpret_cast<std::uintptr_t>(ppData) % kMinMemoryMapAlignment == 0);

    // Step 9: write a sentinel through the source staging. The N2 invariant
    // (locked in by step 11) says these bytes do NOT propagate.
    std::memset(ppData, kSourceWriteByte, kAllocationSize);

    // Step 10: unmap. Drives the same wire round-trip; fake_vkUnmapMemory is
    // called by NonCoherentReceiverAllocation::unmap_endpoint.
    forwarder::generated::vkUnmapMemory_entry(device, memory);
    CHECK(scenario.saw_unmap_memory);

    // Step 11: locked-in Phase 1 invariant — receiver backing store is
    // UNCHANGED from its pre-map fill. Phase 2's flush wiring is what will
    // flip this expectation; any regression that accidentally implements
    // transfer before flush must fail here.
    for (std::size_t i = 0; i < kAllocationSize; ++i) { REQUIRE(g_receiver_backing_store[i] == kReceiverFillByte); }

    // Step 12: free. The forwarder hook drops the manager record at
    // after_pack, so allocation-size is observable as 0 immediately even
    // though vkFreeMemory itself is a deferrable command — it stays in the
    // request stream until the next non-deferrable command (or session
    // teardown) drains it. Direct fake-PFN observation must therefore wait
    // until the queue is flushed; the manager record is the synchronous
    // proof here.
    forwarder::generated::vkFreeMemory_entry(device, memory, nullptr);
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == 0);

    // Step 13: device + instance cleanup. Both are deferrable too; they
    // exist here so the registry's device row is dropped synchronously by
    // the forwarder hook — the matching fake PFNs may never fire because
    // the loopback session is torn down before the deferred chunks are
    // drained. That's fine for Phase 1: the only locked-in cleanup
    // invariants are forwarder-side state, not receiver-side PFN
    // observability.
    forwarder::generated::vkDestroyDevice_entry(device, nullptr);
    forwarder::generated::vkDestroyInstance_entry(instance, nullptr);

    teardown_scenario(scenario);
}

TEST_CASE("phase1 loopback: vkAllocateMemory fallback queries receiver for memory info when registry misses", "[loopback]") {
    // Same fakes as the first test, but with FRESH handle constants so the
    // MemoryTypeRegistry's persistent physical-device rows from the first
    // TEST_CASE cannot mask the miss we want to trigger here. We deliberately
    // skip vkGetPhysicalDeviceProperties and vkGetPhysicalDeviceMemoryProperties
    // so the forwarder's allocate-time resolve() misses — the
    // QueryPhysicalDeviceMemoryInfo manual chunk fires, the receiver answers
    // by calling the real (fake) vkGetPhysicalDevice{Memory,}Properties PFNs,
    // and the registry repopulates. Proving registry population via real
    // receiver dispatch (rather than a synthesized response) is the value-add
    // over the existing memory_info_fallback_test.
    Scenario scenario {
        .instance        = reinterpret_cast<VkInstance>(static_cast<std::uintptr_t>(0xDA77BA11)),
        .physical_device = reinterpret_cast<VkPhysicalDevice>(static_cast<std::uintptr_t>(0xDA77BA12)),
        .device          = reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0xDA77BA13)),
        .memory          = reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(0xDA77BA14)),
    };
    prepare_scenario(scenario);
    sample::VkfwdLoopbackRuntime vkfwd(fake_vkGetInstanceProcAddr);

    VkInstanceCreateInfo instance_ci {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance           instance = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkCreateInstance_entry(&instance_ci, nullptr, &instance) == VK_SUCCESS);

    std::uint32_t physical_device_count = 0;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, nullptr) == VK_SUCCESS);
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    REQUIRE(forwarder::generated::vkEnumeratePhysicalDevices_entry(instance, &physical_device_count, &physical_device) == VK_SUCCESS);

    // Intentionally NO vkGetPhysicalDeviceProperties / vkGetPhysicalDeviceMemoryProperties
    // here — the registry's physical-device rows stay empty so resolve() must miss.
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

    // Observable proof the fallback fired: the receiver invoked the
    // (fake) vkGetPhysicalDevice{Memory,}Properties PFNs to answer the
    // manager's QueryPhysicalDeviceMemoryInfo chunk.
    CHECK(scenario.saw_get_physical_device_properties);
    CHECK(scenario.saw_get_physical_device_memory_properties);

    // The post-fallback retry must classify the allocation; otherwise the
    // bookkeeping size stays zero.
    CHECK(::vkfwd::MemoryMapForwarder::instance().test_get_allocation_size(memory) == kAllocationSize);

    // Cleanup so the next test starts from the registry's empty state.
    forwarder::generated::vkFreeMemory_entry(device, memory, nullptr);
    forwarder::generated::vkDestroyDevice_entry(device, nullptr);
    forwarder::generated::vkDestroyInstance_entry(instance, nullptr);

    teardown_scenario(scenario);
}

} // namespace vkfwd::test
