# Generated Vulkan Code

Every file in this directory tree is produced by
`src/vkfwd/ferry/script/generator/vulkan_metadata.py`. Do not place manual code here; regeneration
may replace these files without preserving local edits.

Per-command generated code and per-command generated metadata live under
`command/`. Human-written hook code belongs under
`src/vkfwd/ferry/core/hook/<api>Hook.hpp` and optional matching `.cpp` files.
`vulkan_api.hpp` contains shared generated API facts such as stable command ids
and the pinned Vulkan API version. There is intentionally no generated
`vulkan_api.cpp`; command metadata and behavior stay per-command.
