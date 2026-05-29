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

  template <class ParameterPacket>
  static constexpr void after_pack(ParameterPacket&) noexcept {}

  template <class ParameterPacket>
  static constexpr void before_unpack(ParameterPacket&) noexcept {}

  template <class Parameters>
  static constexpr void after_unpack(Parameters&) noexcept {}
};

} // namespace vkfwd::manual
