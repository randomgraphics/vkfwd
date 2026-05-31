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
  core/generated/structure/core.cpp
  core/hook/vkCreateDeviceHook.cpp
  core/logging.cpp
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

# Forwarder code is compiled once and packaged two ways: a static archive for
# unit/integration tests that need to drive layer entry points in-process, and
# the Vulkan loader-facing shared object. The shared object still owns the ABI
# boundary; the static archive is a test/linking convenience, not a second layer
# distribution format.
set(VKFWD_FORWARDER_SOURCES
  forwarder/generated/command/vkCreateDevice.cpp
  forwarder/generated/command/vkCreateInstance.cpp
  forwarder/generated/command/vkDestroyDevice.cpp
  forwarder/generated/command/vkDestroyInstance.cpp
  forwarder/generated/dispatch_table.cpp
  forwarder/forwarder.cpp
  forwarder/layer.cpp)

add_library(vkfwd_forwarder_objects OBJECT
  ${VKFWD_FORWARDER_SOURCES})

target_include_directories(vkfwd_forwarder_objects
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/forwarder)

target_link_libraries(vkfwd_forwarder_objects
  PRIVATE
    # The Vulkan loader loads this layer and provides downstream entry points
    # through the layer chain. Linking libvulkan here would add an unnecessary
    # runtime dependency and could blur the dispatch boundary we are testing.
    vkfwd_core)

target_compile_definitions(vkfwd_forwarder_objects
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

set_target_properties(vkfwd_forwarder_objects PROPERTIES
  # The same objects are linked into the shared layer, so they must be PIC even
  # when the consumer is the static test archive.
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

add_library(vkfwd_forwarder_static STATIC
  $<TARGET_OBJECTS:vkfwd_forwarder_objects>)

target_include_directories(vkfwd_forwarder_static
  PUBLIC
    # The static forwarder archive is the in-process test surface for generated
    # layer entry points, so consumers need the forwarder include root. The
    # loader-facing shared object still keeps that include policy private.
    ${CMAKE_CURRENT_SOURCE_DIR}/forwarder)

target_link_libraries(vkfwd_forwarder_static
  PUBLIC
    vkfwd_core)

add_library(vkfwd_forwarder SHARED
  $<TARGET_OBJECTS:vkfwd_forwarder_objects>)

target_link_libraries(vkfwd_forwarder
  PRIVATE
    vkfwd_core)

if(UNIX AND NOT APPLE)
  target_link_options(vkfwd_forwarder
    PRIVATE
      # The layer ABI is the Vulkan loader entry-point pair below. Static
      # archive dependencies are implementation detail and must not become
      # interposable ELF symbols just because they are linked into the .so.
      LINKER:--exclude-libs,ALL)
endif()

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
