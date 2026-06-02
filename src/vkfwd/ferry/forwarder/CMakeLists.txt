# Forwarder code is compiled once and packaged two ways: a static archive for
# unit/integration tests that need to drive layer entry points in-process, and
# the Vulkan loader-facing shared object. The shared object still owns the ABI
# boundary; the static archive is a test/linking convenience, not a second layer
# distribution format.
set(VKFWD_FORWARDER_SOURCES
  generated/forwarder_entrypoints.cpp
  generated/entry/vkCreateDevice_entry.cpp
  generated/entry/vkCreateInstance_entry.cpp
  generated/entry/vkDestroyDevice_entry.cpp
  generated/entry/vkDestroyInstance_entry.cpp
  forwarder.cpp
  layer.cpp)

add_library(vkfwd_forwarder_objects OBJECT
  ${VKFWD_FORWARDER_SOURCES})

target_include_directories(vkfwd_forwarder_objects
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

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
    ${CMAKE_CURRENT_SOURCE_DIR})

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
  manifest/VK_LAYER_VKFWD_forwarder.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_VKFWD_forwarder.json
  @ONLY)
