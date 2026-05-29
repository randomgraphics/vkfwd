# Vulkan Command Hooks

This directory is for human-written hook code that customizes generated Vulkan
capture, packing, unpacking, dispatch, or replay behavior.

Use one clearly named header per Vulkan API command:

```text
src/vkfwd/core/hooks/<api>Hooks.hpp
```

If a hook needs out-of-line logic, add a matching `.cpp` file, for example
`vkCreateDeviceHooks.cpp`, and wire it into CMake manually. Generated command
code conditionally includes hook headers when they exist. The generator must
not create, overwrite, delete, or disable files in this tree. If manual hook
code breaks the build, the build should fail so the hook owner can fix the
command-specific logic.
