# Third-party dependencies are shallow git submodules pinned in .gitmodules. The
# build script initializes them before CMake config, so missing folders should
# fail loudly instead of producing a partial feature set.

set(FMT_INSTALL OFF CACHE BOOL "Disable fmt install rules for vkfwd vendoring" FORCE)
set(FMT_TEST OFF CACHE BOOL "Disable fmt tests for vkfwd vendoring" FORCE)
set(FMT_DOC OFF CACHE BOOL "Disable fmt docs for vkfwd vendoring" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(fmt)

# spdlog and vkfwd should use the same fmt target so formatted strings have a
# single ABI and formatting implementation across logging and non-logging code.
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use vendored fmt target for spdlog" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Disable spdlog install rules for vkfwd vendoring" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Disable spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Disable spdlog tests" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "Disable spdlog benchmarks" FORCE)
add_subdirectory(spdlog)
