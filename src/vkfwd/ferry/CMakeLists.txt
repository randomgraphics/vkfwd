# vkfwd_core holds the shared command model, generated pack/unpack code,
# endpoint contracts, protocol metadata, and small debug implementations. The
# Vulkan layer and receiver link this target so generated payload behavior stays
# identical on both sides of the forwarding boundary.
add_library(vkfwd_core STATIC
  core/blob.cpp
  core/generated/command/vkCreateDevice.cpp
  core/generated/command/vkCreateInstance.cpp
  core/generated/command/vkDestroyDevice.cpp
  core/generated/command/vkDestroyInstance.cpp
  core/hook/vkCreateDeviceHook.cpp
  core/null_api_endpoint.cpp)

set_target_properties(vkfwd_core PROPERTIES
  # Core is linked into the Vulkan layer shared object, so it must be
  # position-independent even though it is currently built as static.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/core)

target_link_libraries(vkfwd_core
  PUBLIC
    vkfwd_vulkan_headers
    fmt::fmt
    spdlog::spdlog)

target_compile_definitions(vkfwd_core
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

# vkfwd_receiver is a separate module because replay owns receiver-only state:
# destination Vulkan dispatch, source-to-destination handle maps, and copied
# command lifetimes.
add_library(vkfwd_receiver STATIC
  receiver/receiver.cpp
  receiver/trace_replay_executor.cpp)

set_target_properties(vkfwd_receiver PROPERTIES
  # Keep this PIC as well so the receiver can later be embedded into a daemon,
  # plugin, or test shared object without changing target semantics.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_receiver
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/receiver)

target_link_libraries(vkfwd_receiver
  PUBLIC
    vkfwd_core)

target_compile_definitions(vkfwd_receiver
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

# The forwarder target should stay small: it exports Vulkan loader entry points
# and delegates payload handling to vkfwd_core. The forwarder-specific singleton
# lives here because it represents layer runtime policy, not receiver replay
# state.
add_library(vkfwd_forwarder SHARED
  forwarder/generated/command/vkCreateDevice.cpp
  forwarder/generated/command/vkCreateInstance.cpp
  forwarder/generated/command/vkDestroyDevice.cpp
  forwarder/generated/command/vkDestroyInstance.cpp
  forwarder/generated/vulkan_forwarder.cpp
  forwarder/forwarder.cpp
  forwarder/layer.cpp)

target_include_directories(vkfwd_forwarder
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/forwarder)

target_link_libraries(vkfwd_forwarder
  PRIVATE
    # The Vulkan loader loads this layer and provides downstream entry points
    # through the layer chain. Linking libvulkan here would add an unnecessary
    # runtime dependency and could blur the dispatch boundary we are testing.
    vkfwd_core)

target_compile_definitions(vkfwd_forwarder
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

set_target_properties(vkfwd_forwarder PROPERTIES
  OUTPUT_NAME "VkLayer_VKFWD_forwarder"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

set(VKFWD_LAYER_LIBRARY_PATH
  "${CMAKE_SHARED_LIBRARY_PREFIX}VkLayer_VKFWD_forwarder${CMAKE_SHARED_LIBRARY_SUFFIX}")

configure_file(
  forwarder/manifest/VK_LAYER_VKFWD_forwarder.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_VKFWD_forwarder.json
  @ONLY)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()
