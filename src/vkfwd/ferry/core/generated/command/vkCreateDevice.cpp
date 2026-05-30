#include "generated/command/vkCreateDevice.hpp"

#include "generated/structure/core.hpp"

#include "logging.hpp"

#include <cstring>
#include <limits>
#include <new>

namespace vkfwd::generated::commands::vkCreateDevice {
namespace {

template<class Pointer>
VkResult patch_command_pointer(Pointer & pointer_slot, std::size_t command_offset, std::size_t target_offset) {
    pointer_slot = reinterpret_cast<Pointer>(target_offset ? static_cast<std::uintptr_t>(target_offset - command_offset) : 0);
    return VK_SUCCESS;
}

VkResult pack_allocator(const VkAllocationCallbacks * allocator, Blob & blob, std::size_t command_offset, const VkAllocationCallbacks *& pointer_slot) {
    if (!allocator) [[unlikely]] { return patch_command_pointer(pointer_slot, command_offset, 0); }
    try {
        auto destination = blob.grow<VkAllocationCallbacks>(1);
        if (!destination.set(0, *allocator)) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy vkCreateDevice allocator callbacks into blob");
            return VK_ERROR_UNKNOWN;
        }
        const std::size_t target = destination.offset();
        return patch_command_pointer(pointer_slot, command_offset, target);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while copying vkCreateDevice allocator callbacks");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

template<class T>
VkResult append_command_chunk(Blob & blob, CommandId command_id, std::uint32_t revision, const T & payload, CommandChunk & chunk, T *& packed_payload) {
    constexpr std::size_t kPayloadAlignment = alignof(T);
    constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kCommandSize      = kPayloadOffset + sizeof(T);
    constexpr std::size_t kChunkAlignment   = alignof(CommandChunkHeader) > kPayloadAlignment ? alignof(CommandChunkHeader) : kPayloadAlignment;

    // The chunk is one contiguous serialized range. Payload starts at an aligned
    // offset inside that range so receivers can safely reinterpret the packed
    // command bytes without depending on host-side append history.
    if constexpr (kCommandSize > std::numeric_limits<std::uint32_t>::max()) {
        VKFWD_LOG_ERROR("vkfwd ferry command pack failed: command chunk is too large, command_id={}, command_size={}", static_cast<std::uint32_t>(command_id),
                        kCommandSize);
        return VK_ERROR_UNKNOWN;
    }

    chunk          = CommandChunk {.command_offset = 0, .command_size = 0};
    packed_payload = nullptr;

    CommandChunkHeader header {};
    try {
        auto destination        = blob.grow<std::uint8_t>(kCommandSize, kChunkAlignment);
        header.command_id       = static_cast<std::uint32_t>(command_id);
        header.size             = static_cast<std::uint32_t>(kCommandSize);
        header.command_revision = revision;

        if (destination.set(0, sizeof(header), reinterpret_cast<const std::uint8_t *>(&header)) != sizeof(header) ||
            destination.set(kPayloadOffset, sizeof(payload), reinterpret_cast<const std::uint8_t *>(&payload)) != sizeof(payload)) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd ferry command pack failed: could not copy command chunk, command_id={}, command_size={}",
                            static_cast<std::uint32_t>(command_id), kCommandSize);
            return VK_ERROR_UNKNOWN;
        }
        chunk.command_offset = destination.offset();
        chunk.command_size   = header.size;
        packed_payload       = reinterpret_cast<T *>(destination.data() + kPayloadOffset);
    } catch (const std::bad_alloc &) {
        VKFWD_LOG_ERROR("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={}, payload_size={}",
                        static_cast<std::uint32_t>(command_id), sizeof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}

template<class T>
VkResult append_command_chunk(Blob & blob, CommandId command_id, std::uint32_t revision, const T & payload, CommandChunk & chunk) {
    T * packed_payload = nullptr;
    return append_command_chunk(blob, command_id, revision, payload, chunk, packed_payload);
}

template<class T>
VkResult unpack_command_chunk(const Blob & blob, const CommandChunk & chunk, CommandId command_id, std::uint32_t revision, const T ** payload) {
    constexpr std::size_t kPayloadAlignment = alignof(T);
    constexpr std::size_t kPayloadOffset    = (sizeof(CommandChunkHeader) + kPayloadAlignment - 1) & ~(kPayloadAlignment - 1);
    constexpr std::size_t kCommandSize      = kPayloadOffset + sizeof(T);
    const auto            header_view       = blob.data_at(chunk.command_offset, sizeof(CommandChunkHeader));
    const auto            payload_view      = blob.data_at(chunk.command_offset + kPayloadOffset, sizeof(T));
    const auto *          header            = reinterpret_cast<const CommandChunkHeader *>(header_view.data());
    const auto *          packed_payload    = reinterpret_cast<const T *>(payload_view.data());
    if (!header || header->command_id != static_cast<std::uint32_t>(command_id) || !packed_payload || header->command_revision != revision ||
        header->size != chunk.command_size || chunk.command_size != kCommandSize) [[unlikely]] {
        VKFWD_LOG_ERROR("vkfwd ferry command unpack failed: invalid command chunk, offset={}, size={}, has_header={}, has_payload={}, command_id={}, "
                        "expected_command_id={}, revision={}, expected_revision={}, header_size={}, expected_size={}",
                        chunk.command_offset, chunk.command_size, header != nullptr, packed_payload != nullptr, header ? header->command_id : 0,
                        static_cast<std::uint32_t>(command_id), header ? header->command_revision : 0, revision, header ? header->size : 0, kCommandSize);
        return VK_ERROR_UNKNOWN;
    }

    *payload = packed_payload;
    return VK_SUCCESS;
}

VkResult pack_into_blob(Blob & blob, const Parameters & parameters, ParameterPacket & packet) {
    // Handles are still source identities at this layer. Replay is responsible
    // for mapping physicalDevice to the receiver-side handle before issuing the
    // real vkCreateDevice call.
    Parameters * packed_parameters = nullptr;
    VkResult     status            = append_command_chunk(blob, CommandId::CreateDevice, kCommandRevision, parameters, packet, packed_parameters);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }

    structure::PackedStruct create_info;
    status = structure::pack_VkDeviceCreateInfo(parameters.pCreateInfo, blob, create_info);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_command_pointer(packed_parameters->pCreateInfo, packet.command_offset, create_info.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_allocator(parameters.pAllocator, blob, packet.command_offset, packed_parameters->pAllocator);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    return VK_SUCCESS;
}

} // namespace

VkResult Command::pack_parameters(Blob & blob, const Parameters & parameters, ParameterPacket & packet) {
    using Hooks = ::vkfwd::manual::CommandHooks<CommandId::CreateDevice>;
    if constexpr (Hooks::before_pack_enabled) {
        Parameters hook_parameters = parameters;
        Hooks::before_pack(hook_parameters);
        VkResult status = pack_into_blob(blob, hook_parameters, packet);
        if (status != VK_SUCCESS) [[unlikely]] { return status; }
    } else {
        VkResult status = pack_into_blob(blob, parameters, packet);
        if (status != VK_SUCCESS) [[unlikely]] { return status; }
    }

    if constexpr (Hooks::after_pack_enabled) { Hooks::after_pack(packet); }
    return VK_SUCCESS;
}

VkResult Command::unpack_parameters(Blob & blob, const ParameterPacket & packet, Parameters & parameters) {
    using Hooks = ::vkfwd::manual::CommandHooks<CommandId::CreateDevice>;
    if constexpr (Hooks::before_unpack_enabled) { Hooks::before_unpack(packet); }

    const Parameters * packed_parameters = nullptr;
    VkResult           status            = unpack_command_chunk(blob, packet, CommandId::CreateDevice, kCommandRevision, &packed_parameters);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    parameters = *packed_parameters;

    if constexpr (Hooks::after_unpack_enabled) { Hooks::after_unpack(parameters); }
    return VK_SUCCESS;
}

VkResult Command::pack_response(Blob & blob, const Response & response, ResponsePacket & packet) {
    return append_command_chunk(blob, CommandId::CreateDevice, kCommandRevision, response, packet);
}

VkResult Command::unpack_response(Blob & blob, const ResponsePacket & packet, Response & response) {
    const Response * packed_response = nullptr;
    VkResult         status          = unpack_command_chunk(blob, packet, CommandId::CreateDevice, kCommandRevision, &packed_response);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    response = *packed_response;
    return VK_SUCCESS;
}

} // namespace vkfwd::generated::commands::vkCreateDevice
