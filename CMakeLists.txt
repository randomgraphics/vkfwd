cmake_minimum_required(VERSION 3.20)

project(vkfwd
  VERSION 0.1.0
  DESCRIPTION "Vulkan API forwarding layer"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

# Vulkan API shape is pinned by the vendored Vulkan-Headers snapshot. Runtime
# linking may still use the platform Vulkan loader, but project compilation and
# generated metadata must agree on this include tree instead of ambient system
# headers.
set(VKFWD_VULKAN_INCLUDE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/vulkan/include")

add_library(vkfwd_vulkan_headers INTERFACE)
target_include_directories(vkfwd_vulkan_headers
  BEFORE INTERFACE
    ${VKFWD_VULKAN_INCLUDE_DIR})

add_subdirectory(src/third_party)
add_subdirectory(src/vkfwd)

if(BUILD_TESTING)
  add_subdirectory(dev/test)
endif()
