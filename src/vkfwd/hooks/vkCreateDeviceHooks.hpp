#pragma once

#include "generated/vulkan_manual_hooks.hpp"

namespace vkfwd::manual {

template <>
struct CommandHooks<vkfwd::generated::CommandId::CreateDevice> {
  static constexpr bool before_pack_enabled = true;
  static constexpr bool after_pack_enabled = false;
  static constexpr bool before_unpack_enabled = false;
  static constexpr bool after_unpack_enabled = false;

  using Parameters = vkfwd::generated::commands::vkCreateDevice::Parameters;

  static void before_pack(Parameters& parameters);

  template <class PackedCommand>
  static constexpr void after_pack(PackedCommand&) noexcept {}

  template <class PackedCommand>
  static constexpr void before_unpack(PackedCommand&) noexcept {}

  template <class Parameters>
  static constexpr void after_unpack(Parameters&) noexcept {}
};

} // namespace vkfwd::manual
