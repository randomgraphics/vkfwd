# Vendored Vulkan Specification Bundle

This directory is the repository's authoritative Vulkan API specification
bundle. `vkfwd` builds generated metadata, generated C++ code, coverage policy,
serialization layout, replay assumptions, and compile-time Vulkan declarations
against the `vk.xml` and headers stored here.

Do not update files in this directory as an isolated dependency refresh.
Changing this bundle is a Vulkan API-version migration for the entire
repository. A migration must update the matching Vulkan headers and `vk.xml`,
regenerate all generated files, revisit command and extension coverage policy,
check stream compatibility notes, and rerun the full build and test suite.

The build must prefer `include/` from this directory over system Vulkan SDK
headers so generated metadata and compiled declarations describe the same API.
