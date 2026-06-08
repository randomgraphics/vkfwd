#pragma once

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <new>

namespace vkfwd::memory_map {

// Append a manual command chunk to a CommandStream. Manual chunks live in the
// same stream as generated ones, so they must use the same chunk-start
// alignment (CommandStream::kBaseAlignment) and the same header-then-payload
// layout the generated append_command_chunk uses. The header carries
// kMemoryMapManagerRevision as command_revision so a wire-format drift between
// forwarder and receiver is detected on the very first chunk.
//
// This helper is shared by every forwarder-side manual-chunk callsite (e.g.
// NonCoherentForwarderAllocation::map / unmap and the
// QueryPhysicalDeviceMemoryInfo fallback) so a future header-layout change
// only has to touch one location.
template<class Payload>
VkResult append_manual_chunk(::vkfwd::CommandStream & stream, ::vkfwd::manual::CommandId command_id, const Payload & payload) {
    constexpr std::size_t kPayloadAlignment = alignof(Payload);
    constexpr std::size_t kPayloadOffset    = (sizeof(::vkfwd::CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kChunkSize        = kPayloadOffset + sizeof(Payload);

    try {
        std::size_t offset      = 0;
        auto        destination = stream.grow<std::uint8_t>(kChunkSize, ::vkfwd::CommandStream::kBaseAlignment, &offset);

        ::vkfwd::CommandChunkHeader header {};
        header.command_id       = static_cast<std::uint32_t>(command_id);
        header.size             = static_cast<std::uint32_t>(kChunkSize);
        header.command_revision = kMemoryMapManagerRevision;
        if (destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t *>(&header)) != sizeof(header) ||
            destination.set(kPayloadOffset, sizeof(payload), reinterpret_cast<const std::uint8_t *>(&payload)) != sizeof(payload)) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd: append_manual_chunk could not copy chunk bytes, command_id={}, payload_size={}", static_cast<std::uint32_t>(command_id),
                            sizeof(Payload));
            return VK_ERROR_UNKNOWN;
        }
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd: append_manual_chunk out of host memory, command_id={}, payload_size={}", static_cast<std::uint32_t>(command_id),
                        sizeof(Payload));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

} // namespace vkfwd::memory_map
