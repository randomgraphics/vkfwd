#pragma once

#include <cstdint>

namespace vkfwd {

constexpr std::uint32_t kStreamMagic = 0x564b4657; // "VKFW"
constexpr std::uint16_t kSupportedWireMajor = 1;
constexpr std::uint16_t kMinimumReadableWireMinor = 0;
constexpr std::uint16_t kMaximumReadableWireMinor = 0;

struct VulkanApiVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;
};

struct WireVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
};

struct Handshake {
  std::uint32_t magic = kStreamMagic;
  WireVersion wire_version;
  VulkanApiVersion vulkan_api_version;
  std::uint32_t generator_schema_version = 0;
};

enum class StreamCompatibility {
  Compatible,
  BadMagic,
  UnsupportedWireMajor,
  UnsupportedWireMinor,
  UnsupportedVulkanMajor,
  NewerVulkanMinor,
};

constexpr StreamCompatibility check_handshake_compatibility(
    const Handshake& incoming,
    VulkanApiVersion receiver_vulkan_api_version) {
  if (incoming.magic != kStreamMagic) {
    return StreamCompatibility::BadMagic;
  }
  if (incoming.wire_version.major != kSupportedWireMajor) {
    return StreamCompatibility::UnsupportedWireMajor;
  }
  if (incoming.wire_version.minor < kMinimumReadableWireMinor ||
      incoming.wire_version.minor > kMaximumReadableWireMinor) {
    return StreamCompatibility::UnsupportedWireMinor;
  }
  if (incoming.vulkan_api_version.major != receiver_vulkan_api_version.major) {
    return StreamCompatibility::UnsupportedVulkanMajor;
  }
  if (incoming.vulkan_api_version.minor > receiver_vulkan_api_version.minor) {
    return StreamCompatibility::NewerVulkanMinor;
  }
  return StreamCompatibility::Compatible;
}

constexpr bool is_compatible_handshake(
    const Handshake& incoming,
    VulkanApiVersion receiver_vulkan_api_version) {
  // Compatibility is negotiated once before command streaming begins. The hot
  // command path relies on the established session version and must not repeat
  // stream-version validation for every Vulkan call.
  return check_handshake_compatibility(incoming, receiver_vulkan_api_version) ==
         StreamCompatibility::Compatible;
}

} // namespace vkfwd
