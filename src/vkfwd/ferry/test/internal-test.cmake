set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  hello-world.cpp
  loopback_session.cpp)

# rapid-vulkan is optional checkout content. Keep the smoke test out of the
# aggregate internal target when its headers are unavailable so core ferry
# protocol tests remain buildable in a minimal source tree.
if(EXISTS "${PROJECT_SOURCE_DIR}/src/third_party/rapid-vulkan/inc/rapid-vulkan/rapid-vulkan.h")
  list(APPEND VKFWD_INTERNAL_TEST_LOCAL_SOURCES
    rapid-vulkan-instance.cpp)
endif()
