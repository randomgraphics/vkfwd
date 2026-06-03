# src/third_party

Place third-party source code and pinned external specification inputs
referenced directly by `src/vkfwd` here.

Keep each dependency in its own subdirectory and include license files with the
vendored source. Prefer system packages or generated code outside this directory
when the dependency does not need to be compiled into the forwarding layer.

Current shallow submodules:

- `fmt`: fmtlib/fmt `12.1.0`
- `glfw`: glfw/glfw `3.4`, the latest stable GLFW 3 release.
- `spdlog`: gabime/spdlog `v1.17.0`
- `rapid-vulkan`: randomgraphics/rapid-vulkan, a header-only C++17 Vulkan
  wrapper imported as source but not wired into the build until vkfwd code
  needs it.
- `vulkan`: Khronos Vulkan-Headers snapshot recorded in `vulkan/VERSION`

Reference-only submodules:

- `gfxreconstruct`: LunarG/gfxreconstruct. This repository solves adjacent
  Vulkan capture/replay problems and is kept here for design and process
  comparison only. It is intentionally not wired into CMake or the forwarding
  runtime because `vkfwd` must preserve real-time forwarding constraints,
  low-overhead interception, and live replay behavior that are stricter than
  offline reconstruction workflows.

Initialize with:

```sh
git submodule update --init --depth 1 src/third_party/fmt src/third_party/glfw src/third_party/spdlog src/third_party/rapid-vulkan
```

Fetch the reference-only submodule separately when it is useful:

```sh
git submodule update --init src/third_party/gfxreconstruct
```
