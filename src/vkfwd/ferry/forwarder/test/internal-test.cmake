# This manifest is consumed by dev/test/internal-test/CMakeLists.txt.
# Keep these handwritten entry-point tests focused on forwarding behavior; core
# generated round-trip tests own exhaustive command and structure serialization.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  getprocaddr_test.cpp
  vkCreateInstance_test.cpp
  vkDestroyInstance_test.cpp
  vkCreateDevice_test.cpp
  vkDestroyDevice_test.cpp
  vkAllocateFreeMemory_test.cpp
  vkMapMemory_test.cpp)
