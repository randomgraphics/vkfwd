# Consumed by dev/test/internal-test/CMakeLists.txt. Tests focus on the
# memory-map manager framework: registry today, per-allocation behavior
# in later phases.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  memory_type_registry_test.cpp
  vm_primitives_test.cpp)
