cmake_minimum_required(VERSION 3.20)

project(vkfwd
  VERSION 0.1.0
  DESCRIPTION "Vulkan API forwarding layer"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_subdirectory(src/contrib)
add_subdirectory(src/vkfwd)

if(BUILD_TESTING)
  add_subdirectory(dev/test)
endif()
