#pragma once

#include <cstddef>
#include <cstdint>

namespace vkfwd {

constexpr std::uint32_t kCommandStreamSchemaVersion = 1;
constexpr std::uint32_t kSupportedSchemaVersion     = kCommandStreamSchemaVersion;
constexpr std::uint32_t kStreamMagic                = 0x564b4657; // "VKFW"
constexpr std::uint32_t kCommandStreamGapMagic      = 0x564b4741; // "VKGA"
constexpr std::size_t   kRequestStreamHeaderSize    = 16;

using StreamId = std::uint64_t;

struct RequestStreamHeader {
    std::uint32_t magic     = kStreamMagic;
    std::uint32_t revision  = kSupportedSchemaVersion;
    StreamId      stream_id = 0;
};
static_assert(sizeof(RequestStreamHeader) == kRequestStreamHeaderSize, "Request stream header layout is part of the ferry wire format");

struct CommandChunkHeader {
    std::uint32_t command_id       = 0;
    std::uint32_t size             = 0;
    std::uint32_t command_revision = 0;
    // Command payloads are appended after this fixed header. Keep the header
    // size a multiple of eight so generated packers can place 64-bit Vulkan
    // handles and pointer-sized fields at naturally aligned payload offsets.
    std::uint32_t padding = 0;
};
static_assert(sizeof(CommandChunkHeader) == 16, "Command chunk header layout is part of the ferry wire format");

struct CommandStreamGapHeader {
    std::uint32_t magic = kCommandStreamGapMagic;
    // Total number of filler bytes from this header through the end of the
    // closed chunk. This record makes chunk tail slack explicit after flattening
    // so receivers never infer protocol structure from allocator state.
    std::uint32_t size = 0;
};

struct Range {
    // The command range is metadata only; all command bytes live in the
    // CommandStream passed beside it. Keeping ownership out of the range makes
    // forwarding and replay choose their own storage lifetime without copying
    // packet wrappers around.
    std::size_t   offset = 0;
    std::uint32_t size   = 0;
};

struct VulkanApiVersion {
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t patch = 0;
};

// Handshake messages are the stable bootstrapping envelope for the generated
// command/structure schema. Schema version is the session-level compatibility
// number; command chunks still carry their own payload revision so one schema
// can support multiple layouts for a specific command.
struct HandshakeRequest {
    std::uint32_t    magic = kStreamMagic;
    VulkanApiVersion vulkan_api_version;
    std::uint32_t    schema_version = 0;
};

struct HandshakeResponse {};

enum class StreamCompatibility {
    Compatible,
    BadMagic,
    UnsupportedSchemaVersion,
    UnsupportedVulkanMajor,
    NewerVulkanMinor,
};

constexpr StreamCompatibility check_handshake_compatibility(const HandshakeRequest & incoming, VulkanApiVersion receiver_vulkan_api_version) {
    if (incoming.magic != kStreamMagic) { return StreamCompatibility::BadMagic; }
    if (incoming.schema_version != kSupportedSchemaVersion) { return StreamCompatibility::UnsupportedSchemaVersion; }
    if (incoming.vulkan_api_version.major != receiver_vulkan_api_version.major) { return StreamCompatibility::UnsupportedVulkanMajor; }
    if (incoming.vulkan_api_version.minor > receiver_vulkan_api_version.minor) { return StreamCompatibility::NewerVulkanMinor; }
    return StreamCompatibility::Compatible;
}

constexpr bool is_compatible_handshake(const HandshakeRequest & incoming, VulkanApiVersion receiver_vulkan_api_version) {
    // Compatibility is negotiated once before command streaming begins. The hot
    // command path relies on the established session version and must not repeat
    // schema-version validation for every Vulkan call.
    return check_handshake_compatibility(incoming, receiver_vulkan_api_version) == StreamCompatibility::Compatible;
}

} // namespace vkfwd
