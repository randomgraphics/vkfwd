#include "protocol.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd {
namespace {

TEST_CASE("handshake accepts matching schema and compatible Vulkan API") {
  const HandshakeRequest request{
      .magic = kStreamMagic,
      .vulkan_api_version = VulkanApiVersion{.major = 1, .minor = 2, .patch = 3},
      .schema_version = kSupportedSchemaVersion,
  };

  CHECK(check_handshake_compatibility(request, VulkanApiVersion{.major = 1, .minor = 3, .patch = 0}) ==
        StreamCompatibility::Compatible);
  CHECK(is_compatible_handshake(request, VulkanApiVersion{.major = 1, .minor = 2, .patch = 0}));
}

TEST_CASE("handshake rejects incompatible stream envelopes before replay") {
  HandshakeRequest request{
      .magic = kStreamMagic,
      .vulkan_api_version = VulkanApiVersion{.major = 1, .minor = 3, .patch = 0},
      .schema_version = kSupportedSchemaVersion,
  };

  SECTION("bad magic") {
    request.magic = 0;
    CHECK(check_handshake_compatibility(request, VulkanApiVersion{.major = 1, .minor = 3, .patch = 0}) ==
          StreamCompatibility::BadMagic);
  }

  SECTION("schema mismatch") {
    request.schema_version = kSupportedSchemaVersion + 1;
    CHECK(check_handshake_compatibility(request, VulkanApiVersion{.major = 1, .minor = 3, .patch = 0}) ==
          StreamCompatibility::UnsupportedSchemaVersion);
  }

  SECTION("major version mismatch") {
    CHECK(check_handshake_compatibility(request, VulkanApiVersion{.major = 2, .minor = 0, .patch = 0}) ==
          StreamCompatibility::UnsupportedVulkanMajor);
  }

  SECTION("sender requires newer minor version") {
    CHECK(check_handshake_compatibility(request, VulkanApiVersion{.major = 1, .minor = 2, .patch = 0}) ==
          StreamCompatibility::NewerVulkanMinor);
  }
}

} // namespace
} // namespace vkfwd
