# Internal C++ tests follow Garnet's single-executable model while letting each
# test directory own its source list. The recursive manifest glob covers the
# repository so new internal-test.cmake files are picked up without editing this
# target, while each manifest still controls which local files are compiled.
file(GLOB_RECURSE VKFWD_INTERNAL_TEST_MANIFESTS CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/*internal-test.cmake")

# Build directories can sit inside a developer checkout. The source manifest
# scan is intentionally repo-wide, but generated build trees must not feed
# stale or configure-time copies back into the test target.
list(FILTER VKFWD_INTERNAL_TEST_MANIFESTS EXCLUDE REGEX "/build[^/]*/")

set(VKFWD_INTERNAL_TEST_SOURCES)
foreach(VKFWD_INTERNAL_TEST_MANIFEST IN LISTS VKFWD_INTERNAL_TEST_MANIFESTS)
  get_filename_component(VKFWD_INTERNAL_TEST_DIR "${VKFWD_INTERNAL_TEST_MANIFEST}" DIRECTORY)
  set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES)
  include("${VKFWD_INTERNAL_TEST_MANIFEST}")

  foreach(VKFWD_INTERNAL_TEST_SOURCE IN LISTS VKFWD_INTERNAL_TEST_LOCAL_SOURCES)
    if(IS_ABSOLUTE "${VKFWD_INTERNAL_TEST_SOURCE}")
      list(APPEND VKFWD_INTERNAL_TEST_SOURCES "${VKFWD_INTERNAL_TEST_SOURCE}")
    else()
      list(APPEND VKFWD_INTERNAL_TEST_SOURCES
        "${VKFWD_INTERNAL_TEST_DIR}/${VKFWD_INTERNAL_TEST_SOURCE}")
    endif()
  endforeach()
endforeach()

list(REMOVE_DUPLICATES VKFWD_INTERNAL_TEST_SOURCES)

add_executable(vkfwd_internal_tests
  ${VKFWD_INTERNAL_TEST_SOURCES})

target_link_libraries(vkfwd_internal_tests
  PRIVATE
    vkfwd_forwarder_static
    vkfwd_core
    Catch2::Catch2WithMain)

add_test(
  NAME vkfwd_internal
  COMMAND vkfwd_internal_tests)

add_test(
  NAME vkfwd_structure
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/script/smoke_structure.sh ${PROJECT_SOURCE_DIR})

add_test(
  NAME vkfwd_generated_metadata
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/script/generated_metadata.sh ${PROJECT_SOURCE_DIR})
