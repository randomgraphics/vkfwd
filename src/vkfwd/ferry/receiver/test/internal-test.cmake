# Receiver-side unit tests. Hook bodies are exercised end-to-end by the
# Phase 1 loopback test (forthcoming); these target structural state on
# ReplayContext that can be checked without a real Vulkan dispatch.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  handle_map_test.cpp
  manual_dispatch_test.cpp
  non_coherent_map_endpoint_test.cpp)
