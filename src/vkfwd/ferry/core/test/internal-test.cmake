# This manifest is consumed by dev/test/internal-test/CMakeLists.txt.
# Keep core-owned regression tests close to the code whose invariants they guard.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  pnext_chain_test.cpp)
