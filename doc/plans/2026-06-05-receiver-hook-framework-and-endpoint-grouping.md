# Receiver Hook Framework and Endpoint Grouping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring the receiver side up to parity with the forwarder's opt-in hook framework, split the monolithic `endpoints.cpp` into per-API-domain files, and migrate the two generator-internal receiver customizations into human-owned hook headers — so per-API receiver behavior lives in `receiver/hook/` and the generator carries no per-API customization data.

**Architecture:** Add `vkfwd::receiver::manual::CommandHooks<CommandId>` (six phases: `before_unpack`, `before_call`, `after_call`, `before_pack_response`, `after_pack_response`, `replace_endpoint`), mirroring `vkfwd::forwarder::manual::CommandHooks`. The generator wires `__has_include("hook/<api>ReceiverHook.hpp")` + `if constexpr (Hooks::<phase>_enabled)` into every receiver endpoint. Endpoint implementations move from one `endpoints.cpp` into per-group `endpoint/<group>.cpp` files driven by a command→group map encoded in `vulkan_metadata.py`; `endpoints.cpp` shrinks to a dispatcher-only `switch`. Finally `vkCreateInstance` / `vkCreateDevice` dispatch-init and `vkUnmapMemory` delegation move into hook headers, and `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`, `receiver_memory_map_endpoint_source`, and `receiver_dispatch_update_lines` are deleted from the generator.

**Tech Stack:** C++20, CMake, Catch2, Vulkan, Python 3 (code generator), clang-format.

**Reference spec:** `doc/receiver-hook-framework-and-endpoint-grouping-design.md` — read it fully before starting. This plan implements its three-step migration.

**Key constraint — generated vs. manual code:** Never hand-edit files under any `generated/` tree. All generated-code changes are edits to `src/vkfwd/ferry/script/generator/vulkan_metadata.py` followed by regeneration. Hook headers under `receiver/hook/` are human-owned and hand-written.

**Regeneration command (run from repo root):**
```sh
python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py
```

**Validation command (run from repo root):**
```sh
python3 dev/bin/cit.py
```
`cit.py` runs `format-all-sources.py --check` first, then the full `vkfwd_internal_tests` Catch2 binary. Hand-written hook headers must pass the format check — run `python3 dev/bin/format-all-sources.py` before committing if needed.

**Build directory:** this machine is macOS; use `build/macos.clang.debug`. `cit.py` auto-selects the newest test binary, so no `--build-dir` flag is normally needed. Build first with `dev/bin/build.py d`.

---

## Decisions locked for this plan (from the design doc's open questions + repo review)

1. **Group manifest location:** encoded directly in `vulkan_metadata.py` as a `COMMAND_GROUP` mapping + `API_GROUP_ORDER` list. No separate module or JSON. (User decision.)
2. **CMake source pickup after split:** `receiver/CMakeLists.txt` uses `file(GLOB ... CONFIGURE_DEPENDS)` over `generated/endpoint/*.cpp`. No hand-edit per new group. (User decision.)
3. **Generator module split (`receiver.py`, etc.):** deferred. Out of scope for this plan. (User decision.)
4. **Hook header naming:** `<api>ReceiverHook.hpp` (parallels `<api>ForwarderHook.hpp`). (Doc draft.)
5. **`replace_endpoint` semantics:** strict mutual exclusion — when `replace_endpoint_enabled` is `true` no other phase fires. (Doc draft.)
6. **Phase wiring by command shape:** `replace_endpoint`, `before_unpack`, `before_call` are wired into every endpoint. `after_call`, `before_pack_response`, `after_pack_response` are wired only into response-bearing endpoints (where a `Command::Response` object exists). This matches the typed signatures in the doc, which all take a `Response`. The two migrated commands (`vkCreateInstance`, `vkCreateDevice`) are response-bearing, so `after_call` has a home. Void endpoints (destroys) silently do not fire the three response-phase hooks; revisit if a void command ever needs one.

---

## File Structure Overview

### Generator (edited, then regenerates everything under `generated/`)
- `src/vkfwd/ferry/script/generator/vulkan_metadata.py` — all generated-code changes.

### Generated files (produced by regeneration — do NOT hand-edit)
- Create: `src/vkfwd/ferry/receiver/generated/receiver_hooks.hpp` — base `CommandHooks<>` template.
- Create: `src/vkfwd/ferry/receiver/generated/endpoint/<group>.hpp` — per-group endpoint declarations.
- Create: `src/vkfwd/ferry/receiver/generated/endpoint/<group>.cpp` — per-group endpoint implementations + hook includes.
- Modify (shape change): `src/vkfwd/ferry/receiver/generated/endpoints.cpp` — becomes dispatcher-only.
- Modify: `src/vkfwd/ferry/receiver/generated/endpoints.hpp` — declares only `call_api_endpoint`.

### Human-owned hook headers (hand-written)
- Create: `src/vkfwd/ferry/receiver/hook/vkCreateInstanceReceiverHook.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkCreateDeviceReceiverHook.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp`

### Build
- Modify: `src/vkfwd/ferry/receiver/CMakeLists.txt` — glob the endpoint sources.

### Tests (hand-written)
- Modify: `src/vkfwd/ferry/test/create-instance-test.cpp` — assert post-`CreateDevice` device dispatch init still works after migration (regression for `after_call`).

---

## Group assignment (the manifest content)

The current 35 supported commands map to these API-domain groups (per `src/vkfwd/ferry/README.md` "Generator Grouping Policy"):

| Group | Commands |
|---|---|
| `global_instance` | vkEnumerateInstanceVersion, vkEnumerateInstanceLayerProperties, vkEnumerateInstanceExtensionProperties, vkCreateInstance, vkDestroyInstance |
| `physical_device` | vkEnumeratePhysicalDevices, vkGetPhysicalDeviceProperties, vkGetPhysicalDeviceFeatures, vkGetPhysicalDeviceQueueFamilyProperties, vkGetPhysicalDeviceMemoryProperties, vkEnumerateDeviceExtensionProperties |
| `device_lifecycle` | vkCreateDevice, vkDestroyDevice, vkGetDeviceQueue, vkDeviceWaitIdle |
| `memory_buffer` | vkCreateBuffer, vkDestroyBuffer, vkGetBufferMemoryRequirements, vkAllocateMemory, vkFreeMemory, vkBindBufferMemory, vkMapMemory, vkUnmapMemory |
| `shader_pipeline_layout` | vkCreateShaderModule, vkDestroyShaderModule, vkCreatePipelineLayout, vkDestroyPipelineLayout, vkCreateGraphicsPipelines, vkDestroyPipeline |
| `renderpass_framebuffer` | vkCreateRenderPass, vkDestroyRenderPass |
| `descriptor` | vkCreateDescriptorSetLayout, vkDestroyDescriptorSetLayout |
| `sync_objects` | vkCreateSemaphore, vkDestroySemaphore |

These are the *receiver endpoint* groups. They intentionally match the command-implementation domain taxonomy so a future command/structure grouping migration can share this same `COMMAND_GROUP` map.

---

## Task 1 — Step 1: Introduce the receiver hook framework

**Outcome:** A generated `receiver_hooks.hpp` base template exists, and every standard receiver endpoint gains opt-in `__has_include` + `if constexpr` hook scaffolding. All `<phase>_enabled` flags default `false`, so compiled behavior is unchanged. `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` and `receiver_dispatch_update_lines` stay in place (removed in Task 3). This task is generator-only + regenerate; no new tests (regression bar = `cit.py` green).

**Files:**
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`
- Regenerates: `src/vkfwd/ferry/receiver/generated/receiver_hooks.hpp` (new), `src/vkfwd/ferry/receiver/generated/endpoints.cpp` (shape change)

- [ ] **Step 1: Add the `receiver_hooks_header_content` emitter**

In `vulkan_metadata.py`, add this function immediately after `forwarder_hooks_header_content` (around line 1330). It mirrors the forwarder base template but with the six receiver phases:

```python
def receiver_hooks_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "generated/vulkan_api.hpp"

namespace vkfwd::receiver::manual {{

// Per-API receiver-endpoint customization. Hook authors specialize this template
// in receiver/hook/<api>ReceiverHook.hpp. The generated endpoint stub conditionally
// calls each phase via `if constexpr (Hooks::<phase>_enabled)`. When
// replace_endpoint_enabled is true the stub bypasses the standard
// unpack/call/pack body entirely and forwards the whole call to replace_endpoint;
// the other phase flags are then ignored.
template <vkfwd::generated::CommandId>
struct CommandHooks {{
  static constexpr bool before_unpack_enabled = false;
  static constexpr bool before_call_enabled = false;
  static constexpr bool after_call_enabled = false;
  static constexpr bool before_pack_response_enabled = false;
  static constexpr bool after_pack_response_enabled = false;
  static constexpr bool replace_endpoint_enabled = false;

  template <class... Args>
  static constexpr void before_unpack(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void before_call(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void after_call(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void before_pack_response(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr void after_pack_response(Args&...) noexcept {{}}

  template <class... Args>
  static constexpr bool replace_endpoint(Args&...) noexcept {{ return false; }}
}};

}} // namespace vkfwd::receiver::manual
"""
```

- [ ] **Step 2: Write `receiver_hooks.hpp` from `write_receiver_files`**

In `write_receiver_files` (around line 2064), add the hooks-header write as the first emission:

```python
def write_receiver_files(metadata: dict[str, object], receiver_dir: Path) -> None:
    receiver_dir.mkdir(parents=True, exist_ok=True)
    (receiver_dir / "receiver_hooks.hpp").write_text(
        receiver_hooks_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.hpp").write_text(
        receiver_endpoints_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.cpp").write_text(
        receiver_endpoints_source_content(metadata), encoding="utf-8"
    )
```

- [ ] **Step 3: Rewrite `receiver_endpoint_source` to wire hook phases**

Replace the existing `receiver_endpoint_source` (lines ~1954-1997) with the hook-aware version below. NOTE: the `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` branch and the `receiver_dispatch_update_lines` call are intentionally KEPT for this task — they are removed in Task 3. The `replace_endpoint` guard, `before_unpack`, and `before_call` are added now; `after_call` / `before_pack_response` / `after_pack_response` are added only on the response-bearing path.

```python
def receiver_endpoint_source(command: dict[str, object]) -> str:
    if str(command["name"]) in RECEIVER_MEMORY_MAP_MANAGED_COMMANDS:
        return receiver_memory_map_endpoint_source(command)
    namespace = command_namespace(str(command["name"]))
    enum_name = command_enum_name(str(command["name"]))
    parameter_names = ", ".join(
        f"parameters->{parameter['name']}" for parameter in command["parameters"]
    )
    pfn_type = command_pfn_type(str(command["name"]))
    fn = receiver_endpoint_function_name(command)

    body: list[str] = []
    if str(command["return_type"]) == "void":
        body.append(f"    api_function({parameter_names});")
    else:
        body.append(
            f"    const {command['return_type']} return_value = api_function({parameter_names});"
        )
        # Kept for Step 1; removed in Step 3 once after_call hooks land.
        for line in receiver_dispatch_update_lines(command):
            body.append("  " + line)

    if command_needs_response(command):
        body.append(f"    Command::Response response {receiver_response_initializer(command)};")
        body.append(
            "    if constexpr (Hooks::after_call_enabled) { Hooks::after_call(*parameters, response, replay_context); }"
        )
        body.append(
            "    if constexpr (Hooks::before_pack_response_enabled) { Hooks::before_pack_response(*parameters, response, replay_context); }"
        )
        body.append("    const bool packed_ok = Command::pack_response(response_stream, response) == VK_SUCCESS;")
        body.append(
            "    if constexpr (Hooks::after_pack_response_enabled) { Hooks::after_pack_response(*parameters, response_stream); }"
        )
        body.append("    return packed_ok;")
    else:
        body.append("    return true;")

    return f"""bool {fn}(const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                               ::vkfwd::receiver::ReplayContext& replay_context) {{
  using Command = ::vkfwd::generated::commands::{namespace}::Command;
  using Hooks = ::vkfwd::receiver::manual::CommandHooks<::vkfwd::generated::CommandId::{enum_name}>;

  if constexpr (Hooks::replace_endpoint_enabled) {{
    // Full override: the endpoint has no standard unpack/call/pack body
    // (e.g. memory-map delegation where no real Vulkan call happens here).
    return Hooks::replace_endpoint(request_stream, request_range, response_stream, replay_context);
  }} else {{
    const auto raw_function = replay_context.dispatch.getProcByCommandId(::vkfwd::generated::CommandId::{enum_name});
    if (!raw_function) {{ return false; }}
    const auto api_function = reinterpret_cast<{pfn_type}>(raw_function);

    auto& mutable_request_stream = const_cast<CommandStream&>(request_stream);
    auto request_view = mutable_request_stream.at(request_range.offset, request_range.size);

    if constexpr (Hooks::before_unpack_enabled) {{ Hooks::before_unpack(request_view, replay_context); }}

    const Command::Parameters* parameters = nullptr;
    if (Command::unpack_parameters(request_view, &parameters) != VK_SUCCESS) {{ return false; }}

    if constexpr (Hooks::before_call_enabled) {{
      Hooks::before_call(*const_cast<Command::Parameters*>(parameters), replay_context);
    }}
{chr(10).join(body)}
  }}
}}
"""
```

- [ ] **Step 4: Add hook includes to the endpoints source TU**

Modify `receiver_endpoints_source_content` (lines ~2031-2061) so the single `endpoints.cpp` includes the hooks base header plus an opt-in `__has_include` block per command. Insert after the command includes:

```python
def receiver_endpoints_source_content(metadata: dict[str, object]) -> str:
    includes = "\n".join(
        f'#include "generated/command/{command["name"]}.hpp"'
        for command in metadata["commands"]
    )
    hook_includes = "\n".join(
        f'#if __has_include("hook/{command["name"]}ReceiverHook.hpp")\n'
        f'#include "hook/{command["name"]}ReceiverHook.hpp"\n'
        f"#endif"
        for command in metadata["commands"]
    )
    endpoints = "\n".join(
        receiver_endpoint_source(command) for command in metadata["commands"]
    )
    dispatch_cases = receiver_endpoint_dispatch_cases(metadata)
    return f"""#include "generated/endpoints.hpp"

{includes}
#include "generated/receiver_hooks.hpp"
#include "memory_map_manager.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

{hook_includes}

namespace vkfwd::receiver::generated {{

{endpoints}
bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                       ::vkfwd::receiver::ReplayContext& replay_context) {{
  switch (command_id) {{
{dispatch_cases}
  }}
  return false;
}}

}} // namespace vkfwd::receiver::generated
"""
```

- [ ] **Step 5: Regenerate**

Run: `python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py`
Expected: `src/vkfwd/ferry/receiver/generated/receiver_hooks.hpp` is created; `endpoints.cpp` now shows `using Hooks = ...` and `if constexpr (Hooks::...)` blocks in each standard endpoint; `vkUnmapMemory_endpoint` still uses the memory-map delegation form (unchanged this task).

- [ ] **Step 6: Build and run the full suite**

Run: `dev/bin/build.py d && python3 dev/bin/cit.py`
Expected: format check passes, build succeeds, all tests pass (PASS). The `if constexpr` blocks compile to nothing since all flags default `false`, so behavior is byte-for-byte equivalent at runtime.

- [ ] **Step 7: Commit**

```bash
git add src/vkfwd/ferry/script/generator/vulkan_metadata.py \
        src/vkfwd/ferry/receiver/generated/receiver_hooks.hpp \
        src/vkfwd/ferry/receiver/generated/endpoints.cpp
git commit -m "$(cat <<'EOF'
Add receiver hook framework scaffolding (opt-in, no behavior change)

Mirrors the forwarder CommandHooks mechanism on the receiver side: generated
base template plus __has_include + if constexpr phase wiring in every endpoint.
All phase flags default false; the two generator-internal customizations stay
in place and are migrated in a later change.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

## Task 2 — Step 2: Split `endpoints.cpp` by API group

**Outcome:** Endpoint implementations move into per-group `endpoint/<group>.cpp` (+ `endpoint/<group>.hpp`), each carrying its own command + hook includes. `endpoints.cpp` becomes a dispatcher-only `switch`. `receiver/CMakeLists.txt` globs the new sources. No semantic change — `cit.py` stays green. `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` / `receiver_dispatch_update_lines` are still present (removed in Task 3).

**Files:**
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`
- Modify: `src/vkfwd/ferry/receiver/CMakeLists.txt`
- Regenerates: `endpoints.cpp` (now dispatcher-only), `endpoints.hpp` (now declares only `call_api_endpoint`), `endpoint/<group>.hpp` + `endpoint/<group>.cpp` (new per-group files)

- [ ] **Step 1: Add the group manifest + lookup helpers**

In `vulkan_metadata.py`, near the other receiver constants (just above `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`, line ~1747), add:

```python
# Receiver endpoint groups, in emission order. Each generated command is assigned
# to exactly one group; endpoint implementations are split into endpoint/<group>.cpp
# so no single source file grows past the ferry-wide grouping limit (see
# src/vkfwd/ferry/README.md "Generator Grouping Policy"). This map is the single
# source of truth a future command/structure grouping migration should also consume.
API_GROUP_ORDER = [
    "global_instance",
    "physical_device",
    "device_lifecycle",
    "memory_buffer",
    "shader_pipeline_layout",
    "renderpass_framebuffer",
    "descriptor",
    "sync_objects",
]

COMMAND_GROUP = {
    "vkEnumerateInstanceVersion": "global_instance",
    "vkEnumerateInstanceLayerProperties": "global_instance",
    "vkEnumerateInstanceExtensionProperties": "global_instance",
    "vkCreateInstance": "global_instance",
    "vkDestroyInstance": "global_instance",
    "vkEnumeratePhysicalDevices": "physical_device",
    "vkGetPhysicalDeviceProperties": "physical_device",
    "vkGetPhysicalDeviceFeatures": "physical_device",
    "vkGetPhysicalDeviceQueueFamilyProperties": "physical_device",
    "vkGetPhysicalDeviceMemoryProperties": "physical_device",
    "vkEnumerateDeviceExtensionProperties": "physical_device",
    "vkCreateDevice": "device_lifecycle",
    "vkDestroyDevice": "device_lifecycle",
    "vkGetDeviceQueue": "device_lifecycle",
    "vkDeviceWaitIdle": "device_lifecycle",
    "vkCreateBuffer": "memory_buffer",
    "vkDestroyBuffer": "memory_buffer",
    "vkGetBufferMemoryRequirements": "memory_buffer",
    "vkAllocateMemory": "memory_buffer",
    "vkFreeMemory": "memory_buffer",
    "vkBindBufferMemory": "memory_buffer",
    "vkMapMemory": "memory_buffer",
    "vkUnmapMemory": "memory_buffer",
    "vkCreateShaderModule": "shader_pipeline_layout",
    "vkDestroyShaderModule": "shader_pipeline_layout",
    "vkCreatePipelineLayout": "shader_pipeline_layout",
    "vkDestroyPipelineLayout": "shader_pipeline_layout",
    "vkCreateGraphicsPipelines": "shader_pipeline_layout",
    "vkDestroyPipeline": "shader_pipeline_layout",
    "vkCreateRenderPass": "renderpass_framebuffer",
    "vkDestroyRenderPass": "renderpass_framebuffer",
    "vkCreateDescriptorSetLayout": "descriptor",
    "vkDestroyDescriptorSetLayout": "descriptor",
    "vkCreateSemaphore": "sync_objects",
    "vkDestroySemaphore": "sync_objects",
}


def command_group(command: dict[str, object]) -> str:
    name = str(command["name"])
    try:
        return COMMAND_GROUP[name]
    except KeyError:
        raise ValueError(
            f"command {name!r} has no receiver endpoint group; add it to COMMAND_GROUP"
        ) from None


def commands_in_group(metadata: dict[str, object], group: str) -> list[dict[str, object]]:
    return [c for c in metadata["commands"] if command_group(c) == group]


def receiver_groups_present(metadata: dict[str, object]) -> list[str]:
    present = {command_group(c) for c in metadata["commands"]}
    return [g for g in API_GROUP_ORDER if g in present]
```

- [ ] **Step 2: Add per-group header + source emitters**

Add these functions next to the other receiver emitters (after `receiver_endpoint_source`, before `receiver_endpoint_dispatch_cases`, ~line 1998):

```python
def receiver_group_header_content(metadata: dict[str, object], group: str) -> str:
    commands = commands_in_group(metadata, group)
    declarations = "\n".join(
        "bool "
        f"{receiver_endpoint_function_name(command)}(const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream, "
        "::vkfwd::receiver::ReplayContext& replay_context);"
        for command in commands
    )
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"
#include "generated/dispatch_table.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::generated {{

{declarations}

}} // namespace vkfwd::receiver::generated
"""


def receiver_group_source_content(metadata: dict[str, object], group: str) -> str:
    commands = commands_in_group(metadata, group)
    includes = "\n".join(
        f'#include "generated/command/{command["name"]}.hpp"' for command in commands
    )
    hook_includes = "\n".join(
        f'#if __has_include("hook/{command["name"]}ReceiverHook.hpp")\n'
        f'#include "hook/{command["name"]}ReceiverHook.hpp"\n'
        f"#endif"
        for command in commands
    )
    endpoints = "\n".join(receiver_endpoint_source(command) for command in commands)
    return f"""#include "generated/endpoint/{group}.hpp"

{includes}
#include "generated/receiver_hooks.hpp"
#include "memory_map_manager.hpp"

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

{hook_includes}

namespace vkfwd::receiver::generated {{

{endpoints}
}} // namespace vkfwd::receiver::generated
"""
```

- [ ] **Step 3: Make `endpoints.cpp` dispatcher-only and `endpoints.hpp` declare only the dispatcher**

Replace `receiver_endpoints_source_content` (the body edited in Task 1 Step 4) with a dispatcher-only version that includes the per-group headers:

```python
def receiver_endpoints_source_content(metadata: dict[str, object]) -> str:
    group_includes = "\n".join(
        f'#include "generated/endpoint/{group}.hpp"'
        for group in receiver_groups_present(metadata)
    )
    dispatch_cases = receiver_endpoint_dispatch_cases(metadata)
    return f"""#include "generated/endpoints.hpp"

{group_includes}

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

namespace vkfwd::receiver::generated {{

bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                       ::vkfwd::receiver::ReplayContext& replay_context) {{
  switch (command_id) {{
{dispatch_cases}
  }}
  return false;
}}

}} // namespace vkfwd::receiver::generated
"""
```

Replace `receiver_endpoints_header_content` so `endpoints.hpp` declares only `call_api_endpoint` (per-endpoint declarations now live in the group headers):

```python
def receiver_endpoints_header_content(metadata: dict[str, object]) -> str:
    return f"""#pragma once

// Generated by src/vkfwd/ferry/script/generator/vulkan_metadata.py; do not edit by hand.
// Vulkan API version: {metadata['versions']['vulkan_api_version']}
// Vulkan XML SHA256: {metadata['generator']['vk_xml_sha256']}

#include "command_stream.hpp"
#include "generated/dispatch_table.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::generated {{

bool call_api_endpoint(::vkfwd::generated::CommandId command_id, const CommandStream& request_stream, const Range& request_range, CommandStream& response_stream,
                       ::vkfwd::receiver::ReplayContext& replay_context);

}} // namespace vkfwd::receiver::generated
"""
```

- [ ] **Step 4: Write the per-group files from `write_receiver_files`**

Replace `write_receiver_files` so it (a) creates the `endpoint/` dir, (b) writes each present group's `.hpp`/`.cpp`, and (c) removes any stale per-group files for groups that no longer have commands:

```python
def write_receiver_files(metadata: dict[str, object], receiver_dir: Path) -> None:
    receiver_dir.mkdir(parents=True, exist_ok=True)
    endpoint_dir = receiver_dir / "endpoint"
    endpoint_dir.mkdir(parents=True, exist_ok=True)

    (receiver_dir / "receiver_hooks.hpp").write_text(
        receiver_hooks_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.hpp").write_text(
        receiver_endpoints_header_content(metadata), encoding="utf-8"
    )
    (receiver_dir / "endpoints.cpp").write_text(
        receiver_endpoints_source_content(metadata), encoding="utf-8"
    )

    present = receiver_groups_present(metadata)
    # Remove stale generated group files so a shrinking manifest never leaves
    # an orphaned .cpp that the CMake glob would still compile.
    for existing in list(endpoint_dir.glob("*.hpp")) + list(endpoint_dir.glob("*.cpp")):
        if existing.stem not in present:
            existing.unlink()
    for group in present:
        (endpoint_dir / f"{group}.hpp").write_text(
            receiver_group_header_content(metadata, group), encoding="utf-8"
        )
        (endpoint_dir / f"{group}.cpp").write_text(
            receiver_group_source_content(metadata, group), encoding="utf-8"
        )
```

- [ ] **Step 5: Confirm the formatter covers the new `endpoint/` tree**

The generator calls `format_generated_files(..., receiver_output_dir)` at the end of `generate()`. Read that function (search `def format_generated_files`) and verify it formats `receiver/generated` recursively (e.g., uses `rglob`/`**`). If it only formats top-level files, add the `endpoint/` subdir to its glob so the new files are clang-formatted. If it already recurses, no change.

Run: `grep -n "def format_generated_files" -A 20 src/vkfwd/ferry/script/generator/vulkan_metadata.py`
Expected: confirm recursion; adjust only if needed.

- [ ] **Step 6: Regenerate**

Run: `python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py`
Expected: `src/vkfwd/ferry/receiver/generated/endpoint/` now contains 8 `.hpp` + 8 `.cpp` files (one per present group); `endpoints.cpp` is reduced to includes + `call_api_endpoint`; `endpoints.hpp` declares only `call_api_endpoint`.

Verify: `ls src/vkfwd/ferry/receiver/generated/endpoint/ && wc -l src/vkfwd/ferry/receiver/generated/endpoints.cpp`
Expected: 16 files listed; `endpoints.cpp` is well under 100 lines.

- [ ] **Step 7: Glob the endpoint sources in `receiver/CMakeLists.txt`**

Edit `src/vkfwd/ferry/receiver/CMakeLists.txt`. Replace the `add_library` source list so it globs the split sources. `CONFIGURE_DEPENDS` makes CMake re-glob on build so a new group file is compiled without a manual reconfigure:

```cmake
# Endpoint implementations are generated one .cpp per API-domain group under
# generated/endpoint/. Glob them so adding a group does not require editing this
# list; CONFIGURE_DEPENDS re-runs the glob at build time when the set changes.
file(GLOB VKFWD_RECEIVER_ENDPOINT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/generated/endpoint/*.cpp")

# vkfwd_ferry_receiver is a separate module because replay owns receiver-only state:
# destination Vulkan dispatch, source-to-destination handle maps, and copied
# command lifetimes.
add_library(vkfwd_ferry_receiver STATIC
  generated/endpoints.cpp
  ${VKFWD_RECEIVER_ENDPOINT_SOURCES}
  receiver.cpp)
```

(Leave the rest of the file — `set_target_properties`, `target_include_directories`, `target_link_libraries`, `target_compile_definitions` — unchanged.)

- [ ] **Step 8: Clean-build and run the full suite**

A glob change requires a fresh configure. Run: `dev/bin/build.py c && dev/bin/build.py d && python3 dev/bin/cit.py`
Expected: format check passes, CMake reconfigures and picks up all 8 group sources, build succeeds, all tests PASS. Behavior is unchanged — this is purely a file-layout change.

- [ ] **Step 9: Commit**

```bash
git add src/vkfwd/ferry/script/generator/vulkan_metadata.py \
        src/vkfwd/ferry/receiver/CMakeLists.txt \
        src/vkfwd/ferry/receiver/generated/endpoints.hpp \
        src/vkfwd/ferry/receiver/generated/endpoints.cpp \
        src/vkfwd/ferry/receiver/generated/endpoint
git commit -m "$(cat <<'EOF'
Split receiver endpoints into per-API-domain group files

endpoints.cpp becomes a dispatcher-only switch; endpoint implementations move
into generated/endpoint/<group>.cpp driven by a COMMAND_GROUP manifest in the
generator. receiver/CMakeLists.txt globs the group sources. No semantic change.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

## Task 3 — Step 3: Migrate customizations into hook headers and delete generator mechanisms

**Outcome:** Three human-owned hook headers land; the generator's `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS`, `receiver_memory_map_endpoint_source`, and `receiver_dispatch_update_lines` are deleted. After regeneration every receiver endpoint follows one code path plus opt-in hooks; no per-API customization data remains in the generator.

**Atomicity requirement:** The hook headers and the generator deletions MUST land together (one commit). If a hook enabling `after_call` for `vkCreateInstance` is committed while `receiver_dispatch_update_lines` still emits the inline `dispatch.instance.init`, the init runs twice. Likewise `vkUnmapMemoryReceiverHook.hpp` has no effect until `vkUnmapMemory` leaves `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS` and uses the standard hook-aware stub.

**Files:**
- Create: `src/vkfwd/ferry/receiver/hook/vkCreateInstanceReceiverHook.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkCreateDeviceReceiverHook.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp`
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`
- Regenerates: `endpoint/global_instance.cpp`, `endpoint/device_lifecycle.cpp`, `endpoint/memory_buffer.cpp`

- [ ] **Step 1: Write `vkCreateInstanceReceiverHook.hpp`**

Create `src/vkfwd/ferry/receiver/hook/vkCreateInstanceReceiverHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkCreateInstance.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateInstance> {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = true;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template<class... Args> static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args> static constexpr void before_call(Args &...) noexcept {}

    static void after_call(const ::vkfwd::generated::commands::vkCreateInstance::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkCreateInstance::Command::Response & response,
                           ::vkfwd::receiver::ReplayContext & replay_context) {
        // Successful receiver-side instance creation changes the destination
        // dispatch scope. Keep this in ReplayContext so tests and transports do
        // not patch host callbacks around the generated endpoint contract.
        if (response.return_value == VK_SUCCESS && parameters.pInstance && *parameters.pInstance) {
            replay_context.dispatch.instance.init(*parameters.pInstance, replay_context.dispatch.global.get_instance_proc_addr);
        }
    }

    template<class... Args> static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args> static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args> static constexpr bool replace_endpoint(Args &...) noexcept { return false; }
};

} // namespace vkfwd::receiver::manual
```

- [ ] **Step 2: Write `vkCreateDeviceReceiverHook.hpp`**

Create `src/vkfwd/ferry/receiver/hook/vkCreateDeviceReceiverHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkCreateDevice.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateDevice> {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = true;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = false;

    template<class... Args> static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args> static constexpr void before_call(Args &...) noexcept {}

    static void after_call(const ::vkfwd::generated::commands::vkCreateDevice::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkCreateDevice::Command::Response & response,
                           ::vkfwd::receiver::ReplayContext & replay_context) {
        // Device dispatch is receiver-owned state derived from the destination
        // device handle. Source-side forwarding must not provide or cache these
        // host function pointers.
        if (response.return_value == VK_SUCCESS && parameters.pDevice && *parameters.pDevice) {
            replay_context.dispatch.device.init(*parameters.pDevice, replay_context.dispatch.instance.get_device_proc_addr);
        }
    }

    template<class... Args> static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args> static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args> static constexpr bool replace_endpoint(Args &...) noexcept { return false; }
};

} // namespace vkfwd::receiver::manual
```

- [ ] **Step 3: Write `vkUnmapMemoryReceiverHook.hpp`**

Create `src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp`. `CommandStream`/`Range` resolve to `vkfwd::CommandStream`/`vkfwd::Range` from `command_stream.hpp`:

```cpp
#pragma once

#include "command_stream.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::UnmapMemory> {
    static constexpr bool before_unpack_enabled        = false;
    static constexpr bool before_call_enabled          = false;
    static constexpr bool after_call_enabled           = false;
    static constexpr bool before_pack_response_enabled = false;
    static constexpr bool after_pack_response_enabled  = false;
    static constexpr bool replace_endpoint_enabled     = true;

    template<class... Args> static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args> static constexpr void before_call(Args &...) noexcept {}
    template<class... Args> static constexpr void after_call(Args &...) noexcept {}
    template<class... Args> static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args> static constexpr void after_pack_response(Args &...) noexcept {}

    static bool replace_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                 ::vkfwd::receiver::ReplayContext & replay_context) {
        // Staging protocol: delegate entirely to the per-context MemoryMapReceiver.
        // The receiver-side mapped pointer stays private to the manager and is
        // never packed into the response stream.
        return replay_context.memoryMap.vkUnmapMemory_endpoint(request_stream, request_range, response_stream);
    }
};

} // namespace vkfwd::receiver::manual
```

- [ ] **Step 4: Delete `receiver_dispatch_update_lines` and its use**

In `vulkan_metadata.py`:
- Delete the entire `receiver_dispatch_update_lines` function (was ~lines 1889-1921).
- In `receiver_endpoint_source` (the version from Task 1 Step 3), delete the two lines that call it:
  ```python
          # Kept for Step 1; removed in Step 3 once after_call hooks land.
          for line in receiver_dispatch_update_lines(command):
              body.append("  " + line)
  ```
  After deletion the `else` branch for non-void return is just:
  ```python
      else:
          body.append(
              f"    const {command['return_type']} return_value = api_function({parameter_names});"
          )
  ```

- [ ] **Step 5: Delete the memory-map managed-command mechanism**

In `vulkan_metadata.py`:
- Delete the `RECEIVER_MEMORY_MAP_MANAGED_COMMANDS = {"vkUnmapMemory"}` constant (and its comment, ~lines 1745-1747).
- Delete the entire `receiver_memory_map_endpoint_source` function (~lines 1933-1951).
- In `receiver_endpoint_source`, delete the leading branch:
  ```python
      if str(command["name"]) in RECEIVER_MEMORY_MAP_MANAGED_COMMANDS:
          return receiver_memory_map_endpoint_source(command)
  ```
  so the function now starts directly at `namespace = command_namespace(...)`.

- [ ] **Step 6: Verify no dangling references**

Run: `grep -n "RECEIVER_MEMORY_MAP_MANAGED_COMMANDS\|receiver_memory_map_endpoint_source\|receiver_dispatch_update_lines" src/vkfwd/ferry/script/generator/vulkan_metadata.py`
Expected: no matches.

- [ ] **Step 7: Regenerate**

Run: `python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py`
Expected:
- `endpoint/memory_buffer.cpp`: `vkUnmapMemory_endpoint` is now the standard hook-aware stub whose `if constexpr (Hooks::replace_endpoint_enabled)` branch (now `true` via the hook) returns `replay_context.memoryMap.vkUnmapMemory_endpoint(...)`.
- `endpoint/global_instance.cpp`: `vkCreateInstance_endpoint` no longer has inline `dispatch.instance.init`; the work happens via `Hooks::after_call`.
- `endpoint/device_lifecycle.cpp`: same for `vkCreateDevice_endpoint`.

Verify: `grep -rn "dispatch.instance.init\|dispatch.device.init" src/vkfwd/ferry/receiver/generated/`
Expected: no matches (those calls now live only in the hook headers under `receiver/hook/`).

- [ ] **Step 8: Build and run the full suite**

Run: `dev/bin/build.py d && python3 dev/bin/cit.py`
Expected: format check passes (hook headers included), build succeeds, all tests PASS. The loopback tests (`test/create-instance-test.cpp`) exercise receiver instance/device dispatch init — they prove `after_call` fires. The memory-map test (`forwarder/test/vkMapMemory_test.cpp`) exercises unmap delegation — it proves `replace_endpoint` fires.

- [ ] **Step 9: Commit (hooks + generator deletions together)**

```bash
git add src/vkfwd/ferry/receiver/hook/vkCreateInstanceReceiverHook.hpp \
        src/vkfwd/ferry/receiver/hook/vkCreateDeviceReceiverHook.hpp \
        src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp \
        src/vkfwd/ferry/script/generator/vulkan_metadata.py \
        src/vkfwd/ferry/receiver/generated/endpoint
git commit -m "$(cat <<'EOF'
Migrate receiver customizations to hook headers; drop generator mechanisms

vkCreateInstance/vkCreateDevice dispatch-init move to after_call hooks and
vkUnmapMemory delegation moves to a replace_endpoint hook. Removes
RECEIVER_MEMORY_MAP_MANAGED_COMMANDS, receiver_memory_map_endpoint_source, and
receiver_dispatch_update_lines so the generator carries no per-API receiver
customization data.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

## Task 4 — Regression assertion for migrated `after_call` (device dispatch)

**Outcome:** An explicit loopback assertion that a device-level call replays after `vkCreateDevice`, proving the `after_call` hook initialized device dispatch. This guards the migration without adding tests against unused phases.

**Files:**
- Modify: `src/vkfwd/ferry/test/create-instance-test.cpp` (or add a sibling test file with its own `internal-test.cmake` entry if a device-level loopback fixture does not already exist)

- [ ] **Step 1: Read the existing loopback fixture**

Run: `sed -n '40,120p' src/vkfwd/ferry/test/create-instance-test.cpp`
Determine whether the existing fixture already drives `vkCreateDevice` and a device-level call through the loopback runtime. If it already asserts a successful device-level replay, this task is satisfied by the existing coverage — mark it done after confirming `cit.py` is green, and skip the remaining steps. If it only covers instance creation, continue.

- [ ] **Step 2: Add a device-dispatch replay assertion**

If needed, extend the loopback test so that after a successful `vkCreateDevice_entry`, a device-level entry point (e.g. `vkDeviceWaitIdle_entry` or `vkGetDeviceQueue_entry`) is forwarded and replays successfully. The fake host loader must return a real function pointer for the device-level command so `replay_context.dispatch.device` must have been initialized for the call to reach it. Follow the existing `fake_vkGetInstanceProcAddr` / `fake_vkCreateInstance` pattern in the same file: add a `fake_vkGetDeviceProcAddr`, a `fake_vkCreateDevice` that returns a sentinel `VkDevice`, and a `fake_vkDeviceWaitIdle` that records it was called; then `CHECK` the recorded flag is set after the loopback round-trip.

(The exact wiring depends on the fixture shape you read in Step 1 — model new fakes on the existing ones in this file. Do not invent a new harness; reuse `sample::VkfwdLoopbackRuntime`.)

- [ ] **Step 3: Build and run the targeted test**

Run: `dev/bin/build.py d && ./build/macos.clang.debug/dev/test/internal-test/vkfwd_internal_tests "[loopback]"`
Expected: all `[loopback]` cases PASS, including the device-dispatch assertion.

- [ ] **Step 4: Full suite + commit**

Run: `python3 dev/bin/cit.py`
Expected: PASS.

```bash
git add src/vkfwd/ferry/test/create-instance-test.cpp
git commit -m "$(cat <<'EOF'
Assert device-level replay after vkCreateDevice in loopback test

Locks in the after_call hook migration: a device-level command only replays if
receiver-side device dispatch was initialized by the vkCreateDevice after_call
hook.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Deferred / explicitly out of scope

- **Synthetic all-phase hook test.** The design doc proposes one test toggling every `<phase>_enabled` flag and asserting call order. It is deferred: `before_unpack`, `before_call`, `before_pack_response`, and `after_pack_response` have NO real users after this work, and hooks bind to endpoints at library-compile time (a test cannot inject a hook into an already-compiled endpoint without committing a production hook header). Testing them would mean either a copied skeleton that rots out of sync with the generator, or polluting production with a counting hook — both violate the doc's own rule (line 272): "Tests land alongside the real behavior they cover, not against empty scaffolding." The two phases with real users (`after_call`, `replace_endpoint`) ARE covered by Task 3/4 regressions. Revisit when a real consumer for one of the unused phases lands.
- **Generator module split** (`receiver.py` etc.) — deferred per decision 3.
- **Forwarder hook framework, core pack/unpack hooks, dispatcher representation, group taxonomy definition** — non-goals per the design doc.

## Self-Review (completed by plan author)

- **Spec coverage:** Step 1 (hook framework) → Task 1. Step 2 (endpoint split + group manifest) → Task 2. Step 3 (migrate 3 customizations + delete 3 generator mechanisms) → Task 3. Doc test plan → Task 3/4 regressions + documented deferral of the synthetic test. Generator-module-split guidance → deferred per user. Open questions 1-3 → resolved in "Decisions locked."
- **Type/name consistency:** `CommandHooks` phase flags and method names match across the base template (Task 1), the three specializations (Task 3), and the generated `if constexpr` call sites (Task 1 Step 3). `command_group` / `commands_in_group` / `receiver_groups_present` / `COMMAND_GROUP` / `API_GROUP_ORDER` are used consistently across Task 2. `replace_endpoint` signature `(const CommandStream&, const Range&, CommandStream&, ReplayContext&) -> bool` matches the endpoint signature and the `vkUnmapMemory` hook. `after_call(const Parameters&, const Response&, ReplayContext&)` matches both create hooks and the generated response-path call site.
- **Placeholders:** none — every code/step block contains concrete content.
