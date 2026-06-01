add_library(vkfwd_loopback STATIC
  loopback.cpp)

target_include_directories(vkfwd_loopback
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(vkfwd_loopback
  PUBLIC
    vkfwd_core vkfwd_forwarder_static)
