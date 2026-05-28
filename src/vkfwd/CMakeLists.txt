find_package(Vulkan REQUIRED)

# vkfwd_capture is separated from the loadable layer so serialization logic can
# be unit-tested without involving Vulkan loader discovery.
add_library(vkfwd_capture STATIC
  forwarder.cpp
  null_forwarding_sink.cpp
  text_call_serializer.cpp)

set_target_properties(vkfwd_capture PROPERTIES
  # The capture library is linked into the Vulkan layer shared object, so it
  # must be position-independent even though it is currently built as static.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_capture
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(vkfwd_capture
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

# vkfwd_receiver is a separate module because replay owns different state from
# capture: destination Vulkan dispatch, source-to-destination handle maps, and
# deserialized object lifetimes.
add_library(vkfwd_receiver STATIC
  receiver.cpp
  text_call_deserializer.cpp
  trace_replay_executor.cpp)

set_target_properties(vkfwd_receiver PROPERTIES
  # Keep this PIC as well so the receiver can later be embedded into a daemon,
  # plugin, or test shared object without changing target semantics.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_receiver
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vkfwd_receiver
  PUBLIC
    fmt::fmt
    spdlog::spdlog)

target_compile_definitions(vkfwd_receiver
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

# The layer target should stay small: it exports Vulkan loader entry points and
# delegates capture details to vkfwd_capture.
add_library(vkfwd_layer SHARED
  layer.cpp)

target_include_directories(vkfwd_layer
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS})

target_link_libraries(vkfwd_layer
  PRIVATE
    vkfwd_capture
    Vulkan::Vulkan)

target_link_libraries(vkfwd_capture
  PUBLIC
    fmt::fmt
    spdlog::spdlog)

target_compile_definitions(vkfwd_layer
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")

set_target_properties(vkfwd_layer PROPERTIES
  OUTPUT_NAME "VkLayer_VKFWD_forwarder"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

set(VKFWD_LAYER_LIBRARY_PATH
  "${CMAKE_SHARED_LIBRARY_PREFIX}VkLayer_VKFWD_forwarder${CMAKE_SHARED_LIBRARY_SUFFIX}")

configure_file(
  manifest/VK_LAYER_VKFWD_forwarder.json.in
  ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_VKFWD_forwarder.json
  @ONLY)
