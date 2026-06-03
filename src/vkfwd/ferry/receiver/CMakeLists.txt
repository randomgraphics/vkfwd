# vkfwd_ferry_receiver is a separate module because replay owns receiver-only state:
# destination Vulkan dispatch, source-to-destination handle maps, and copied
# command lifetimes.
add_library(vkfwd_ferry_receiver STATIC
  generated/endpoints.cpp
  receiver.cpp)

set_target_properties(vkfwd_ferry_receiver PROPERTIES
  # Keep this PIC as well so the receiver can later be embedded into a daemon,
  # plugin, or test shared object without changing target semantics.
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(vkfwd_ferry_receiver
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vkfwd_ferry_receiver
  PUBLIC
    vkfwd_ferry_core)

target_compile_definitions(vkfwd_ferry_receiver
  PRIVATE
    VKFWD_VERSION="${PROJECT_VERSION}")
