#pragma once

#include <cstdint>

namespace vkfwd {

constexpr std::uint32_t kStreamMagic = 0x564b4657; // "VKFW"
constexpr std::uint32_t kSupportedSchemaVersion = 1;

struct VulkanApiVersion {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t patch = 0;
};

struct CommandChunkHeader {
  std::uint32_t command_id = 0;
  std::uint32_t size = 0;
  std::uint32_t command_revision = 0;
};

// Handshake messages are the stable bootstrapping envelope for the generated
// command/structure schema. Schema version is the session-level compatibility
// number; command chunks still carry their own payload revision so one schema
// can support multiple layouts for a specific command.
struct HandshakeRequest {
  std::uint32_t magic = kStreamMagic;
  VulkanApiVersion vulkan_api_version;
  std::uint32_t schema_version = 0;
};

struct HandshakeResponse {};

enum class StreamCompatibility {
  Compatible,
  BadMagic,
  UnsupportedSchemaVersion,
  UnsupportedVulkanMajor,
  NewerVulkanMinor,
};

constexpr StreamCompatibility check_handshake_compatibility(
    const HandshakeRequest& incoming,
    VulkanApiVersion receiver_vulkan_api_version) {
  if (incoming.magic != kStreamMagic) {
    return StreamCompatibility::BadMagic;
  }
  if (incoming.schema_version != kSupportedSchemaVersion) {
    return StreamCompatibility::UnsupportedSchemaVersion;
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
  // schema-version validation for every Vulkan call.
  return check_handshake_compatibility(incoming, receiver_vulkan_api_version) ==
         StreamCompatibility::Compatible;
}

} // namespace vkfwd
