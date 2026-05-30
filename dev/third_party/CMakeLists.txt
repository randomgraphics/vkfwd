# dev/third_party is for tools and tests only. Production targets should depend
# on src/third_party so development-only packages cannot leak into shipping
# linkage or install policy.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catch2/CMakeLists.txt")
  message(FATAL_ERROR
    "Catch2 submodule is required for BUILD_TESTING=ON. "
    "Run: git submodule update --init --depth 1 dev/third_party/catch2")
endif()

set(CATCH_INSTALL_DOCS OFF CACHE BOOL "Disable Catch2 docs install rules" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "Disable Catch2 extras install rules" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "Disable Catch2 self tests" FORCE)
set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "Disable Catch2 examples" FORCE)
add_subdirectory(catch2 EXCLUDE_FROM_ALL)
