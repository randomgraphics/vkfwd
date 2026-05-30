#include "generated/command/vkCreateDevice.hpp"

#include "generated/structure/core.hpp"

#if __has_include(<spdlog/spdlog.h>)
    #include <spdlog/spdlog.h>
#else
namespace spdlog {
template<class... Args>
void error(const char *, Args &&...) noexcept {}
} // namespace spdlog
#endif

#include <cstring>
#include <new>

namespace vkfwd::generated::commands::vkCreateDevice {
namespace {

VkResult patch_command_pointer(Blob & blob, std::size_t command_offset, std::size_t field_offset, std::size_t target_offset) {
    const std::uintptr_t encoded = target_offset ? static_cast<std::uintptr_t>(target_offset - command_offset) : 0;
    if (!blob.overwrite_bytes(command_offset + sizeof(CommandChunkHeader) + field_offset, &encoded, sizeof(encoded))) [[unlikely]] {
        spdlog::error("vkfwd ferry command pack failed: could not patch vkCreateDevice pointer field, command_offset={}, field_offset={}, target_offset={}",
                      command_offset, field_offset, target_offset);
        return VK_ERROR_UNKNOWN;
    }
    return VK_SUCCESS;
}

VkResult pack_allocator(const VkAllocationCallbacks * allocator, Blob & blob, std::size_t command_offset, std::size_t field_offset) {
    if (!allocator) [[unlikely]] { return patch_command_pointer(blob, command_offset, field_offset, 0); }
    try {
        const std::size_t target = blob.append_bytes(allocator, sizeof(*allocator), alignof(*allocator));
        return patch_command_pointer(blob, command_offset, field_offset, target);
    } catch (const std::bad_alloc &) {
        spdlog::error("vkfwd ferry command pack failed: out of host memory while copying vkCreateDevice allocator callbacks");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

template<class T>
VkResult append_command_chunk(Blob & blob, CommandId command_id, std::uint32_t revision, const T & payload, CommandChunk & chunk) {
    const std::size_t command_offset = blob.next_offset();
    chunk                           = CommandChunk {.command_offset = command_offset, .command_size = 0};

    CommandChunkHeader header {};
    try {
        blob.append_value(header, alignof(CommandChunkHeader));
        blob.append_value(payload, alignof(T));
    } catch (const std::bad_alloc &) {
        spdlog::error("vkfwd ferry command pack failed: out of host memory while creating command chunk, command_id={}, payload_size={}",
                      static_cast<std::uint32_t>(command_id), sizeof(T));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    header.command_id        = static_cast<std::uint32_t>(command_id);
    header.size              = static_cast<std::uint32_t>(blob.next_offset() - command_offset);
    header.command_revision  = revision;
    if (!blob.overwrite_bytes(command_offset, &header, sizeof(header))) [[unlikely]] {
        spdlog::error("vkfwd ferry command pack failed: could not write command chunk header, command_id={}, command_offset={}, command_size={}",
                      static_cast<std::uint32_t>(command_id), command_offset, header.size);
        return VK_ERROR_UNKNOWN;
    }
    chunk.command_size = header.size;
    return VK_SUCCESS;
}

template<class T>
VkResult unpack_command_chunk(const Blob & blob, const CommandChunk & chunk, CommandId command_id, std::uint32_t revision, const T ** payload) {
    const auto * header = reinterpret_cast<const CommandChunkHeader *>(blob.data_at(chunk.command_offset, sizeof(CommandChunkHeader)));
    const auto * packed_payload = reinterpret_cast<const T *>(blob.data_at(chunk.command_offset + sizeof(CommandChunkHeader), sizeof(T)));
    if (!header || header->command_id != static_cast<std::uint32_t>(command_id) || !packed_payload || header->command_revision != revision ||
        header->size != chunk.command_size) [[unlikely]] {
        spdlog::error(
            "vkfwd ferry command unpack failed: invalid command chunk, offset={}, size={}, has_header={}, has_payload={}, command_id={}, "
            "expected_command_id={}, revision={}, expected_revision={}, header_size={}",
            chunk.command_offset, chunk.command_size, header != nullptr, packed_payload != nullptr, header ? header->command_id : 0,
            static_cast<std::uint32_t>(command_id), header ? header->command_revision : 0, revision, header ? header->size : 0);
        return VK_ERROR_UNKNOWN;
    }

    *payload = packed_payload;
    return VK_SUCCESS;
}

VkResult pack_into_blob(Blob & blob, const Parameters & parameters, ParameterPacket & packet) {
    // Handles are still source identities at this layer. Replay is responsible
    // for mapping physicalDevice to the receiver-side handle before issuing the
    // real vkCreateDevice call.
    VkResult status = append_command_chunk(blob, CommandId::CreateDevice, kCommandRevision, parameters, packet);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }

    structure::PackedStruct create_info;
    status = structure::pack_VkDeviceCreateInfo(parameters.pCreateInfo, blob, create_info);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = patch_command_pointer(blob, packet.command_offset, offsetof(Parameters, pCreateInfo), create_info.offset);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    status = pack_allocator(parameters.pAllocator, blob, packet.command_offset, offsetof(Parameters, pAllocator));
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
    VkResult           status = unpack_command_chunk(blob, packet, CommandId::CreateDevice, kCommandRevision, &packed_parameters);
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
    VkResult         status = unpack_command_chunk(blob, packet, CommandId::CreateDevice, kCommandRevision, &packed_response);
    if (status != VK_SUCCESS) [[unlikely]] { return status; }
    response = *packed_response;
    return VK_SUCCESS;
}

} // namespace vkfwd::generated::commands::vkCreateDevice
