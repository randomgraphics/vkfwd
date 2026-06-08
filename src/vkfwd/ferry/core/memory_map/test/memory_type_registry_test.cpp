#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::memory_map::test {
namespace {

template <class T>
T test_handle(std::uintptr_t v) {
    return reinterpret_cast<T>(v);
}

VkPhysicalDeviceMemoryProperties make_two_type_props() {
    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 2;
    props.memoryTypes[0] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    props.memoryTypes[1] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    return props;
}

// The registry is a process singleton; tests reuse it. forget_device drops the
// per-device association so unrelated tests don't observe each other's records.
void clear_registry(MemoryTypeRegistry & registry, VkDevice device) {
    registry.forget_device(device);
}

} // namespace

TEST_CASE("MemoryTypeRegistry::resolve returns recorded property flags and atom size") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x101);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x201);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);

    const auto resolved_non_coherent = registry.resolve(device, 0);
    REQUIRE(resolved_non_coherent.has_value());
    CHECK(resolved_non_coherent->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    CHECK(resolved_non_coherent->non_coherent_atom_size == 64);
    CHECK(resolved_non_coherent->min_memory_map_alignment == 4096);

    const auto resolved_coherent = registry.resolve(device, 1);
    REQUIRE(resolved_coherent.has_value());
    CHECK((resolved_coherent->property_flags
           & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);

    clear_registry(registry, device);
}

TEST_CASE("MemoryTypeRegistry::resolve returns nullopt before preconditions are recorded") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x102);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x202);

    // Device known, but no memory properties yet.
    registry.record_device(device, physical_device);
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // Memory properties present, but no atom size yet.
    registry.record_memory_properties(physical_device, make_two_type_props());
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // Atom size alone is not enough; mapped pointers also have an alignment
    // contract from VkPhysicalDeviceLimits::minMemoryMapAlignment.
    registry.record_non_coherent_atom_size(physical_device, 64);
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // All preconditions present: resolves.
    registry.record_min_memory_map_alignment(physical_device, 4096);
    CHECK(registry.resolve(device, 0).has_value());

    clear_registry(registry, device);
}

TEST_CASE("MemoryTypeRegistry::resolve returns nullopt for out-of-range memory type") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x103);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x203);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);

    CHECK_FALSE(registry.resolve(device, 7).has_value());

    clear_registry(registry, device);
}

TEST_CASE("MemoryTypeRegistry::forget_device drops the device->physical mapping") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x104);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x204);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);
    REQUIRE(registry.resolve(device, 0).has_value());

    registry.forget_device(device);
    CHECK_FALSE(registry.resolve(device, 0).has_value());
}

} // namespace vkfwd::memory_map::test
