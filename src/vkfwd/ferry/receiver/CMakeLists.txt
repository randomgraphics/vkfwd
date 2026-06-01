# vkfwd_receiver is a separate module because replay owns receiver-only state:
# destination Vulkan dispatch, source-to-destination handle maps, and copied
# command lifetimes.
add_library(vkfwd_receiver STATIC
  receiver.cpp
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
    vkfwd_core)

target_compile_definitions(vkfwd_receiver
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")
