#pragma once

#include "wire/wire_1_0.hpp"

#include <cstdint>

namespace vkfwd {

constexpr std::uint32_t kStreamMagic = 0x564b4657; // "VKFW"
constexpr std::uint16_t kSupportedWireMajor = wire_1_0::kWireMajor;
constexpr std::uint16_t kMinimumReadableWireMinor = wire_1_0::kWireMinor;
constexpr std::uint16_t kMaximumReadableWireMinor = wire_1_0::kWireMinor;

struct VulkanApiVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;
};

struct WireVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
};

// Handshake messages are the stable bootstrapping envelope for all later wire
// revisions. They must remain backward and forward compatible: add optional
// fields only when old peers can safely ignore them, and never reinterpret or
// remove existing fields.
struct HandshakeRequest {
  std::uint32_t magic = kStreamMagic;
  WireVersion wire_version;
  VulkanApiVersion vulkan_api_version;
  std::uint32_t generator_schema_version = 0;
};

struct HandshakeResponse {};

enum class StreamCompatibility {
  Compatible,
  BadMagic,
  UnsupportedWireMajor,
  UnsupportedWireMinor,
  UnsupportedVulkanMajor,
  NewerVulkanMinor,
};

constexpr StreamCompatibility check_handshake_compatibility(
    const HandshakeRequest& incoming,
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
    const HandshakeRequest& incoming,
    VulkanApiVersion receiver_vulkan_api_version) {
  // Compatibility is negotiated once before command streaming begins. The hot
  // command path relies on the established session version and must not repeat
  // stream-version validation for every Vulkan call.
  return check_handshake_compatibility(incoming, receiver_vulkan_api_version) ==
         StreamCompatibility::Compatible;
}

} // namespace vkfwd
