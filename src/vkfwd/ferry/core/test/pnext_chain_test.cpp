#include "generated/structure/core.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace vkfwd::generated::structure {
namespace {

VkPhysicalDeviceFeatures2 make_features2(void * pnext = nullptr) {
    return VkPhysicalDeviceFeatures2 {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = pnext,
        .features = {},
    };
}

VkPhysicalDeviceVulkan11Features make_vulkan11_features(void * pnext = nullptr) {
    return VkPhysicalDeviceVulkan11Features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = pnext,
    };
}

void * invalid_application_pointer() {
    // The validation path should reject obviously unreadable application memory
    // before generated pNext packers dereference it. This sentinel is only used
    // on POSIX-like builds where the validator installs a guarded fault probe.
    return reinterpret_cast<void *>(std::uintptr_t {1});
}

} // namespace

TEST_CASE("pNext chain packing accepts null and supported chains") {
    Blob         blob;
    PackedStruct packed;

    CHECK(pack_pnext_chain(nullptr, blob, packed) == VK_SUCCESS);
    CHECK(packed.offset == 0);

    auto tail = make_vulkan11_features();
    auto head = make_features2(&tail);

    CHECK(pack_pnext_chain(&head, blob, packed) == VK_SUCCESS);
    CHECK(blob.size() > 0);
}

TEST_CASE("pNext chain packing rejects unsupported structure types") {
    Blob              blob;
    PackedStruct      packed;
    VkBaseInStructure unsupported {
        .sType = static_cast<VkStructureType>(0x7fffffff),
        .pNext = nullptr,
    };

    CHECK(pack_pnext_chain(&unsupported, blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);

    auto head = make_features2(&unsupported);
    CHECK(pack_pnext_chain(&head, blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);
}

TEST_CASE("pNext chain packing rejects cycles before copying payload") {
    Blob         blob;
    PackedStruct packed;

    auto self_cycle  = make_features2();
    self_cycle.pNext = &self_cycle;

    CHECK(pack_pnext_chain(&self_cycle, blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);

    auto first  = make_features2();
    auto second = make_vulkan11_features(&first);
    first.pNext = &second;

    CHECK(pack_pnext_chain(&first, blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);
}

TEST_CASE("pNext chain packing rejects chains beyond the validation depth limit") {
    Blob         blob;
    PackedStruct packed;

    std::vector<VkPhysicalDeviceFeatures2> chain(1001);
    for (std::size_t index = 0; index < chain.size(); ++index) { chain[index] = make_features2(index + 1 < chain.size() ? &chain[index + 1] : nullptr); }

    CHECK(pack_pnext_chain(chain.data(), blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);
}

TEST_CASE("pNext chain packing rejects unreadable node pointers without crashing") {
#if defined(__unix__) || defined(__APPLE__)
    Blob         blob;
    PackedStruct packed;

    CHECK(pack_pnext_chain(invalid_application_pointer(), blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);

    auto head = make_features2(invalid_application_pointer());
    CHECK(pack_pnext_chain(&head, blob, packed) == VK_ERROR_UNKNOWN);
    CHECK(packed.offset == 0);
#endif
}

TEST_CASE("packed pNext unpacking rejects corrupt blob references") {
    Blob blob;

    CHECK(unpack_pnext_chain(blob, 0, nullptr) == VK_ERROR_UNKNOWN);

    const void * unpacked = reinterpret_cast<const void *>(0x1);
    CHECK(unpack_pnext_chain(blob, 4096, &unpacked) == VK_ERROR_UNKNOWN);
    CHECK(unpacked == nullptr);
}

} // namespace vkfwd::generated::structure
