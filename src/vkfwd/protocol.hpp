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

struct StreamHeader {
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

constexpr StreamCompatibility check_stream_compatibility(
    const StreamHeader& incoming,
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

constexpr bool is_compatible_stream(
    const StreamHeader& incoming,
    VulkanApiVersion receiver_vulkan_api_version) {
  // The receiver/replay runtime is the long-lived compatibility boundary. A
  // stream is accepted only when the wire format is in the readable range and
  // Vulkan stays within the same major API line; command-specific replay can
  // still reject unknown commands or unsupported payload revisions later.
  return check_stream_compatibility(incoming, receiver_vulkan_api_version) ==
         StreamCompatibility::Compatible;
}

} // namespace vkfwd
