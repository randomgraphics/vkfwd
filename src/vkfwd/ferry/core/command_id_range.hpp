#pragma once

#include <cstdint>

namespace vkfwd {

// Generated and manual command ids share one 32-bit space in
// CommandChunkHeader::command_id. To make collisions structurally impossible,
// the upper region [kReservedCommandIdBase, 2^32) is reserved for manual ids
// owned by vkfwd; the generator constrains its hash output to stay strictly
// below this base, and every generated per-command header static-asserts that
// invariant. Manual ids in core/custom_command.hpp static-assert the
// complementary invariant.
//
// 128 K reserved slots is far more than vkfwd will ever need for custom wire
// commands; the cost on the generated side is one part in ~32 K of the hash
// space, which is irrelevant for ~600-Vulkan-command-class workloads.
constexpr std::uint32_t kReservedCommandIdBase = 0xFFFE0000u;

} // namespace vkfwd
