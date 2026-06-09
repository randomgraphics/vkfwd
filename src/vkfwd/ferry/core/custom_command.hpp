#pragma once

#include "command_id_range.hpp"

#include <cstdint>

namespace vkfwd::manual {

// CustomCommandId values share the CommandChunkHeader::command_id field with
// generated Vulkan CommandId values. Manual ids are constrained to live in the
// reserved upper region [kReservedCommandIdBase, 2^32); the generator
// constrains its hash output to stay strictly below that base, and per-command
// static_asserts on both sides catch any drift. Keep all custom ids here so
// receiver dispatch can reserve and audit this protocol surface in one place.
enum class CommandId : std::uint32_t {
    MemoryMap                     = 0xFFFE0001u,
    MemoryUnmap                   = 0xFFFE0002u,
    QueryPhysicalDeviceMemoryInfo = 0xFFFE0003u,
    MemoryFlush                   = 0xFFFE0004u,
    MemoryInvalidate              = 0xFFFE0005u,
};

static_assert(static_cast<std::uint32_t>(CommandId::MemoryMap) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::MemoryUnmap) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::QueryPhysicalDeviceMemoryInfo) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::MemoryFlush) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::MemoryInvalidate) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");

} // namespace vkfwd::manual
