set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  create-instance-test.cpp)
set_source_files_properties("${VKFWD_INTERNAL_TEST_DIR}/create-instance-test.cpp"
  PROPERTIES
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/third_party/rapid-vulkan/inc")
