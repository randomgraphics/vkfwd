#include "generated/command/vkCreateInstance.hpp"

#include "generated/structure/core.hpp"

#include <cstring>
#include <new>

namespace vkfwd::generated::commands::vkCreateInstance {
namespace {

VkResult patch_command_pointer(Blob * blob, std::size_t command_offset, std::size_t field_offset, std::size_t target_offset) {
    const std::uintptr_t encoded = target_offset ? static_cast<std::uintptr_t>(target_offset - command_offset) : 0;
    if (!blob->overwrite_bytes(command_offset + sizeof(CommandChunkHeader) + field_offset, &encoded, sizeof(encoded))) { return VK_ERROR_UNKNOWN; }
    return VK_SUCCESS;
}

VkResult pack_allocator(const VkAllocationCallbacks * allocator, Blob * blob, std::size_t command_offset, std::size_t field_offset) {
    if (!allocator) { return patch_command_pointer(blob, command_offset, field_offset, 0); }
    try {
        const std::size_t target = blob->append_bytes(allocator, sizeof(*allocator), alignof(*allocator));
        return patch_command_pointer(blob, command_offset, field_offset, target);
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }
}

VkResult pack_into_blob(const Parameters & parameters, ParameterPacket * packet) {
    *packet            = ParameterPacket {};
    packet->command_id = CommandId::CreateInstance;
    packet->parameters = parameters;

    Blob &            blob           = packet->blob;
    const std::size_t command_offset = blob.next_offset();
    packet->command_offset           = command_offset;

    CommandChunkHeader header {};
    try {
        blob.append_value(header, alignof(CommandChunkHeader));
        // Command-argument pointer slots are patched to command-relative offsets.
        // Nested struct helpers switch to struct-relative offsets at the struct
        // chunk boundary, so a copied struct can later move independently.
        blob.append_value(parameters, alignof(Parameters));
    } catch (const std::bad_alloc &) { return VK_ERROR_OUT_OF_HOST_MEMORY; }

    structure::PackedStruct create_info;
    VkResult                status = structure::pack_VkInstanceCreateInfo(parameters.pCreateInfo, &blob, &create_info);
    if (status != VK_SUCCESS) { return status; }
    status = patch_command_pointer(&blob, command_offset, offsetof(Parameters, pCreateInfo), create_info.offset);
    if (status != VK_SUCCESS) { return status; }
    status = pack_allocator(parameters.pAllocator, &blob, command_offset, offsetof(Parameters, pAllocator));
    if (status != VK_SUCCESS) { return status; }

    header.command_id = static_cast<std::uint32_t>(CommandId::CreateInstance);
    header.size       = static_cast<std::uint32_t>(blob.next_offset() - command_offset);
    header.command_revision = kCommandRevision;
    if (!blob.overwrite_bytes(command_offset, &header, sizeof(header))) { return VK_ERROR_UNKNOWN; }
    packet->command_size = header.size;
    return VK_SUCCESS;
}

} // namespace

VkResult Command::pack_parameters(const Parameters & parameters, ParameterPacket * packet) {
    if (!packet) { return VK_ERROR_UNKNOWN; }

    using Hooks = ::vkfwd::manual::CommandHooks<CommandId::CreateInstance>;
    if constexpr (Hooks::before_pack_enabled) {
        Parameters hook_parameters = parameters;
        Hooks::before_pack(hook_parameters);
        VkResult status = pack_into_blob(hook_parameters, packet);
        if (status != VK_SUCCESS) { return status; }
    } else {
        VkResult status = pack_into_blob(parameters, packet);
        if (status != VK_SUCCESS) { return status; }
    }

    if constexpr (Hooks::after_pack_enabled) { Hooks::after_pack(*packet); }
    return VK_SUCCESS;
}

VkResult Command::unpack_parameters(const ParameterPacket & packet, Parameters * parameters) {
    if (!parameters) { return VK_ERROR_UNKNOWN; }

    using Hooks = ::vkfwd::manual::CommandHooks<CommandId::CreateInstance>;
    if constexpr (Hooks::before_unpack_enabled) { Hooks::before_unpack(packet); }

    const auto * header = reinterpret_cast<const CommandChunkHeader *>(packet.blob.data_at(packet.command_offset, sizeof(CommandChunkHeader)));
    const auto * packed_parameters =
        reinterpret_cast<const Parameters *>(packet.blob.data_at(packet.command_offset + sizeof(CommandChunkHeader), sizeof(Parameters)));
    if (!header || !packed_parameters || header->command_id != static_cast<std::uint32_t>(CommandId::CreateInstance) ||
        header->command_revision != kCommandRevision) {
        return VK_ERROR_UNKNOWN;
    }
    *parameters = *packed_parameters;

    if constexpr (Hooks::after_unpack_enabled) { Hooks::after_unpack(*parameters); }
    return VK_SUCCESS;
}

VkResult Command::pack_response(const Response & response, ResponsePacket * packet) {
    if (!packet) { return VK_ERROR_UNKNOWN; }
    *packet = ResponsePacket {CommandId::CreateInstance, response};
    return VK_SUCCESS;
}

VkResult Command::unpack_response(const ResponsePacket & packet, Response * response) {
    if (!response) { return VK_ERROR_UNKNOWN; }
    *response = packet.response;
    return VK_SUCCESS;
}

} // namespace vkfwd::generated::commands::vkCreateInstance
