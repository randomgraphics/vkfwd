# Memory Map Manager Skeleton (Phase 0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reshape `MemoryMapForwarder` / `MemoryMapReceiver` into a per-allocation polymorphic framework so future phase-1+ strategies can land as self-contained subclasses without churning the manager, hooks, or `ReplayContext`. No user-visible behavior change in this phase: `vkMapMemory` continues to return `VK_ERROR_FEATURE_NOT_PRESENT`.

**Architecture:** Add a `core/memory_map/` subdirectory with abstract `ForwarderAllocation` / `ReceiverAllocation` base classes, empty `NonCoherent*` / `Coherent*` placeholder subclasses, a `MemoryTypeRegistry` cache fed by four new forwarder hooks, centralized manual command ids in `vkfwd::manual::CommandId`, and factories that pick the subclass from `VkMemoryPropertyFlags`. The existing `MemoryMapForwarder` / `MemoryMapReceiver` facades stay in the `vkfwd::` namespace and become thin dispatch over `std::unordered_map<VkDeviceMemory, std::unique_ptr<*Allocation>>`. The receiver-side per-handle map exists but stays empty in phase 0 — phase 1 populates it through the custom memory-map command path (`manual::CommandId::MemoryMap` / `MemoryUnmap`), not through standard generated Vulkan map/unmap chunks.

**Tech Stack:** C++20, CMake, Catch2 for tests, Vulkan, spdlog.

**Reference spec:** `doc/memory_map_management.md` — read the "Phase 0 — Skeleton Framework" section before starting any task.

**Validation command:** `python3 dev/bin/cit.py` (runs format-check, then the full `vkfwd_internal_tests` Catch2 binary). Run from repo root. Do **not** use `ctest`.

**Test policy:** Every test in this plan is a pure CPU-side mockup. They construct Vulkan structs as plain data, drive forwarder entry points through the existing `install_pack_unpack_transport` test transport, and synthesize fake receiver responses. No test invokes a real Vulkan loader or driver in any task. When future phases need real Vulkan validation (e.g., end-to-end loopback against real `vkMapMemory` / `vkUnmapMemory`), use the project's `rapid-vulkan` plus loopback-runtime integration — but no such test is required in phase 0.

---

## File Structure Overview

### Files to delete

- `src/vkfwd/ferry/core/memory_map_manager.hpp`
- `src/vkfwd/ferry/core/memory_map_manager.cpp`

### Files to create — `core/memory_map/` tree

```
src/vkfwd/ferry/core/memory_map/
    manager.hpp                              # vkfwd::MemoryMapForwarder + vkfwd::MemoryMapReceiver public facades
    manager.cpp
    memory_type_registry.hpp                 # vkfwd::memory_map::MemoryTypeRegistry (forwarder-side cache)
    memory_type_registry.cpp
    forwarder_allocation.hpp                 # vkfwd::memory_map::ForwarderAllocation (abstract base)
    forwarder_allocation.cpp
    forwarder_allocation_factory.hpp         # vkfwd::memory_map::ForwarderAllocationFactory
    forwarder_allocation_factory.cpp
    forwarder/
        non_coherent_allocation.hpp          # NonCoherentForwarderAllocation (placeholder)
        non_coherent_allocation.cpp
        coherent_allocation.hpp              # CoherentForwarderAllocation (placeholder)
        coherent_allocation.cpp
    receiver_allocation.hpp                  # vkfwd::memory_map::ReceiverAllocation (abstract base)
    receiver_allocation.cpp
    receiver_allocation_factory.hpp
    receiver_allocation_factory.cpp
    receiver/
        non_coherent_allocation.hpp          # NonCoherentReceiverAllocation (placeholder)
        non_coherent_allocation.cpp
        coherent_allocation.hpp              # CoherentReceiverAllocation (placeholder)
        coherent_allocation.cpp
    test/
        internal-test.cmake
        memory_type_registry_test.cpp
```

### Files to create — manual command ids

```
src/vkfwd/ferry/core/custom_command.hpp        # vkfwd::manual::CommandId for vkfwd-owned wire commands
```

### Files to create — forwarder hook headers

```
src/vkfwd/ferry/forwarder/hook/
    vkCreateDeviceForwarderHook.hpp
    vkDestroyDeviceForwarderHook.hpp
    vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp
    vkGetPhysicalDevicePropertiesForwarderHook.hpp
```

### Files to create — forwarder test

```
src/vkfwd/ferry/forwarder/test/memory_type_registry_hooks_test.cpp
```

### Files to create / update — receiver fail-closed hooks for obsolete standard map commands

```
src/vkfwd/ferry/receiver/hook/vkMapMemoryReceiverHook.hpp      # reject generated standard vkMapMemory chunks
src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp    # reject generated standard vkUnmapMemory chunks
```

### Files to modify

- `src/vkfwd/ferry/core/CMakeLists.txt` — replace `memory_map_manager.cpp` with the new `memory_map/...` source set.
- `src/vkfwd/ferry/script/generator/vulkan_metadata.py` — change `#include "memory_map_manager.hpp"` emission to `#include "memory_map/manager.hpp"` (two sites near lines 1716 and 2043).
- `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp` — pass `property_flags`, `memory_type_index`, `non_coherent_atom_size`, and `min_memory_map_alignment` to `record_allocation` after resolving via `MemoryTypeRegistry`.
- `src/vkfwd/ferry/forwarder/hook/vkFreeMemoryForwarderHook.hpp` — update include path only.
- `src/vkfwd/ferry/forwarder/hook/vkMapMemoryForwarderHook.hpp` — update include path only.
- `src/vkfwd/ferry/forwarder/hook/vkUnmapMemoryForwarderHook.hpp` — update include path only.
- `src/vkfwd/ferry/receiver/replay_context.hpp` — update include path only.
- `src/vkfwd/ferry/forwarder/test/vkAllocateFreeMemory_test.cpp` — prime `MemoryTypeRegistry` before calling `vkAllocateMemory_entry`.
- `src/vkfwd/ferry/forwarder/test/internal-test.cmake` — add `memory_type_registry_hooks_test.cpp`.

### Files that will be regenerated

- `src/vkfwd/ferry/forwarder/generated/entry/vkMapMemory_entry.cpp` / `vkUnmapMemory_entry.cpp` — once phase 1 adds these APIs to `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS`, generated public Vulkan entrypoints delegate to `MemoryMapForwarder::custom_vkMapMemory_entry` / `custom_vkUnmapMemory_entry`, which emit manual command ids instead of generated Vulkan map/unmap payloads.
- `src/vkfwd/ferry/receiver/generated/endpoints.cpp` — generator-emitted include becomes `memory_map/manager.hpp`.

---

## Namespace Convention

- `MemoryMapForwarder`, `MemoryMapReceiver` (the public facades) stay in `vkfwd::`. Callers (`replay_context.hpp`, hook headers, tests) do not change their use of `::vkfwd::MemoryMapForwarder` / `::vkfwd::MemoryMapReceiver`.
- All new internal classes go in `vkfwd::memory_map::` — `ForwarderAllocation`, `ReceiverAllocation`, `MemoryTypeRegistry`, the two factories, and the four placeholder subclasses.
- Manual vkfwd-owned wire command ids live in `vkfwd::manual::CommandId` in `core/custom_command.hpp`. This mirrors `vkfwd::generated::CommandId` for standard Vulkan XML-generated commands but keeps hand-written protocol ids visibly separate.

This keeps caller churn limited to include-path updates.

---

## Task 1A — Add centralized manual command ids

**Status: already complete.** The file landed in commit `61614ca`
("Refine memory-map design and reserve a manual command-id range") with
the intended content (reserved ids in `[kReservedCommandIdBase, 2^32)`
plus matching `static_assert`s).

**Files (for reference, do not recreate):**
- `src/vkfwd/ferry/core/custom_command.hpp` — exists.
- `src/vkfwd/ferry/core/command_id_range.hpp` — exists.

- [x] **Step 1: Verify the in-tree header still matches the design**

Treat this as a verification step, not a creation step. Run the diff
below; if it produces output, reconcile against the design in
`doc/memory_map_management.md` (the "Command-id namespace invariant"
section) before continuing — the rest of the plan assumes these ids
and asserts are in place.

```bash
diff <(cat <<'EOF'
#pragma once

#include "command_id_range.hpp"

#include <cstdint>

namespace vkfwd::manual {

// CustomCommandId values share the CommandChunkHeader::command_id field with
// generated Vulkan CommandId values. Manual ids are constrained to live in the
// reserved upper region [kReservedCommandIdBase, 2^32); the generator
// constrains its hash output to stay strictly below that base, and per-command
// static_asserts on both sides catch any drift. Keep all custom ids here so
// receiver dispatch can reserve and audit this protocol surface in one place.
enum class CommandId : std::uint32_t {
    MemoryMap                     = 0xFFFE0001u,
    MemoryUnmap                   = 0xFFFE0002u,
    QueryPhysicalDeviceMemoryInfo = 0xFFFE0003u,
};

static_assert(static_cast<std::uint32_t>(CommandId::MemoryMap) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::MemoryUnmap) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");
static_assert(static_cast<std::uint32_t>(CommandId::QueryPhysicalDeviceMemoryInfo) >= ::vkfwd::kReservedCommandIdBase,
              "manual command id must live in the reserved range [kReservedCommandIdBase, 2^32)");

} // namespace vkfwd::manual
EOF
) src/vkfwd/ferry/core/custom_command.hpp
```

- [x] **Step 2: No commit.** The file is already committed upstream
(commit `61614ca`). Skip to Task 1B.

---

## Task 1B — Fail closed for obsolete standard generated map/unmap commands

**Files:**
- Create: `src/vkfwd/ferry/receiver/hook/vkMapMemoryReceiverHook.hpp`
- Modify: `src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp`

The public Vulkan `vkMapMemory` / `vkUnmapMemory` entry points will use
vkfwd's manual memory-map protocol. Standard generated Vulkan
`CommandId::MapMemory` / `CommandId::UnmapMemory` chunks are therefore obsolete
for receiver replay and must not silently call the real driver.

- [ ] **Step 1: Add `vkMapMemoryReceiverHook.hpp`**

Create a receiver hook with `replace_endpoint_enabled = true` that logs:

```text
vkfwd receiver: standard generated vkMapMemory command is disabled; use manual::CommandId::MemoryMap
```

and returns `false`. The hook must not call real `vkMapMemory` and must not pack
a response.

- [ ] **Step 2: Update `vkUnmapMemoryReceiverHook.hpp`**

Make the existing standard `vkUnmapMemory` receiver hook also
`replace_endpoint_enabled = true`, log:

```text
vkfwd receiver: standard generated vkUnmapMemory command is disabled; use manual::CommandId::MemoryUnmap
```

and return `false`. Do not delegate this standard command to
`MemoryMapReceiver`; phase 1 dispatches `manual::CommandId::MemoryUnmap`
directly to `custom_vkUnmapMemory_endpoint`.

- [ ] **Step 3: Commit**

```bash
git add src/vkfwd/ferry/receiver/hook/vkMapMemoryReceiverHook.hpp \
        src/vkfwd/ferry/receiver/hook/vkUnmapMemoryReceiverHook.hpp
git commit -m "WIP reject standard generated map memory receiver commands"
```

---

## Task 1 — Relocate the manager files into `core/memory_map/`

**Files:**
- Delete: `src/vkfwd/ferry/core/memory_map_manager.hpp`
- Delete: `src/vkfwd/ferry/core/memory_map_manager.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/manager.hpp` (verbatim move of `memory_map_manager.hpp`)
- Create: `src/vkfwd/ferry/core/memory_map/manager.cpp` (verbatim move of `memory_map_manager.cpp`)
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`
- Modify: `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp`
- Modify: `src/vkfwd/ferry/forwarder/hook/vkFreeMemoryForwarderHook.hpp`
- Modify: `src/vkfwd/ferry/forwarder/hook/vkMapMemoryForwarderHook.hpp`
- Modify: `src/vkfwd/ferry/forwarder/hook/vkUnmapMemoryForwarderHook.hpp`
- Modify: `src/vkfwd/ferry/receiver/replay_context.hpp`
- Modify: `src/vkfwd/ferry/forwarder/test/vkAllocateFreeMemory_test.cpp`
- Regenerate: `src/vkfwd/ferry/receiver/generated/endpoints.cpp`

- [ ] **Step 1: Create the destination directory**

```bash
mkdir -p src/vkfwd/ferry/core/memory_map
```

- [ ] **Step 2: Move the manager files**

```bash
git mv src/vkfwd/ferry/core/memory_map_manager.hpp src/vkfwd/ferry/core/memory_map/manager.hpp
git mv src/vkfwd/ferry/core/memory_map_manager.cpp src/vkfwd/ferry/core/memory_map/manager.cpp
```

- [ ] **Step 3: Update `src/vkfwd/ferry/core/memory_map/manager.cpp` so its own `#include` line points at the new header**

Find the line `#include "memory_map_manager.hpp"` near the top and change it to:

```cpp
#include "memory_map/manager.hpp"
```

- [ ] **Step 4: Update `src/vkfwd/ferry/core/CMakeLists.txt`**

Find the `memory_map_manager.cpp` line in the `add_library(vkfwd_ferry_core STATIC ...)` source list and change it to:

```cmake
memory_map/manager.cpp
```

- [ ] **Step 5: Update every consumer's include path**

In each of the following files, change `#include "memory_map_manager.hpp"` to `#include "memory_map/manager.hpp"`:

- `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp`
- `src/vkfwd/ferry/forwarder/hook/vkFreeMemoryForwarderHook.hpp`
- `src/vkfwd/ferry/forwarder/hook/vkMapMemoryForwarderHook.hpp`
- `src/vkfwd/ferry/forwarder/hook/vkUnmapMemoryForwarderHook.hpp`
- `src/vkfwd/ferry/receiver/replay_context.hpp`
- `src/vkfwd/ferry/forwarder/test/vkAllocateFreeMemory_test.cpp`

- [ ] **Step 6: Update the generator's two include-emission sites**

In `src/vkfwd/ferry/script/generator/vulkan_metadata.py`:

- Near line 1716 (in `forwarder_memory_map_command_source_content`): change the emitted `#include "memory_map_manager.hpp"` to `#include "memory_map/manager.hpp"`.
- Near line 2043 (in `receiver_endpoints_source_content`): same change.

Also update the doc comment at line 1937 from "memory_map_manager.hpp" to "memory_map/manager.hpp" so it does not lie about the include path.

- [ ] **Step 7: Regenerate**

```bash
python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py
```

Verify `src/vkfwd/ferry/receiver/generated/endpoints.cpp` now has `#include "memory_map/manager.hpp"` near the top.

- [ ] **Step 8: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests pass (977 assertions in 83 test cases as of the WIP checkpoint baseline). If any test fails, the rename missed an include or the CMake source list still references the old path.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP relocate memory_map_manager into core/memory_map/manager

Pure file move plus include-path updates. The single-file
memory_map_manager.{hpp,cpp} becomes core/memory_map/manager.{hpp,cpp}
to make room for the per-allocation framework that lands next. No
behavior change.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Add `MemoryTypeRegistry`

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/memory_type_registry.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/memory_type_registry.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`
- Create: `src/vkfwd/ferry/core/memory_map/test/memory_type_registry_test.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Create `src/vkfwd/ferry/core/memory_map/test/memory_type_registry_test.cpp`:

```cpp
#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::memory_map::test {
namespace {

template <class T>
T test_handle(std::uintptr_t v) {
    return reinterpret_cast<T>(v);
}

VkPhysicalDeviceMemoryProperties make_two_type_props() {
    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 2;
    props.memoryTypes[0] = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    props.memoryTypes[1] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    return props;
}

void clear_registry(MemoryTypeRegistry & registry,
                    VkDevice device, VkPhysicalDevice physical_device) {
    // Avoid cross-test pollution from the process-singleton registry by
    // re-seeding inside each test and forgetting the device at the end.
    registry.forget_device(device);
}

} // namespace

TEST_CASE("MemoryTypeRegistry::resolve returns recorded property flags and atom size") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x101);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x201);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);

    const auto resolved_non_coherent = registry.resolve(device, 0);
    REQUIRE(resolved_non_coherent.has_value());
    CHECK(resolved_non_coherent->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    CHECK(resolved_non_coherent->non_coherent_atom_size == 64);
    CHECK(resolved_non_coherent->min_memory_map_alignment == 4096);

    const auto resolved_coherent = registry.resolve(device, 1);
    REQUIRE(resolved_coherent.has_value());
    CHECK((resolved_coherent->property_flags
           & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0);

    clear_registry(registry, device, physical_device);
}

TEST_CASE("MemoryTypeRegistry::resolve returns nullopt before preconditions are recorded") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x102);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x202);

    // Device known, but no memory properties yet.
    registry.record_device(device, physical_device);
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // Memory properties present, but no atom size yet.
    registry.record_memory_properties(physical_device, make_two_type_props());
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // Atom size alone is not enough; mapped pointers also have an alignment
    // contract from VkPhysicalDeviceLimits::minMemoryMapAlignment.
    registry.record_non_coherent_atom_size(physical_device, 64);
    CHECK_FALSE(registry.resolve(device, 0).has_value());

    // All preconditions present: resolves.
    registry.record_min_memory_map_alignment(physical_device, 4096);
    CHECK(registry.resolve(device, 0).has_value());

    clear_registry(registry, device, physical_device);
}

TEST_CASE("MemoryTypeRegistry::resolve returns nullopt for out-of-range memory type") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x103);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x203);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);

    CHECK_FALSE(registry.resolve(device, 7).has_value());

    clear_registry(registry, device, physical_device);
}

TEST_CASE("MemoryTypeRegistry::forget_device drops the device->physical mapping") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   device          = test_handle<VkDevice>(0x104);
    auto   physical_device = test_handle<VkPhysicalDevice>(0x204);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_two_type_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);
    REQUIRE(registry.resolve(device, 0).has_value());

    registry.forget_device(device);
    CHECK_FALSE(registry.resolve(device, 0).has_value());
}

} // namespace vkfwd::memory_map::test
```

- [ ] **Step 2: Create the test manifest**

Create `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`:

```cmake
# Consumed by dev/test/internal-test/CMakeLists.txt. Tests focus on the
# memory-map manager framework: registry today, per-allocation behavior
# in later phases.
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  memory_type_registry_test.cpp)
```

- [ ] **Step 3: Verify the test fails at compile time**

```bash
python3 dev/bin/cit.py
```

Expected: build fails on `memory_type_registry_test.cpp` because `memory_map/memory_type_registry.hpp` does not exist yet. This is the "test exists and fails" gate.

- [ ] **Step 4: Add the header**

Create `src/vkfwd/ferry/core/memory_map/memory_type_registry.hpp`:

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace vkfwd::memory_map {

// Forwarder-side cache populated by hooks on vkCreateDevice,
// vkDestroyDevice, vkGetPhysicalDeviceMemoryProperties, and
// vkGetPhysicalDeviceProperties. Allocate-time code calls resolve() to
// translate (device, memoryTypeIndex) into the property flags and
// nonCoherentAtomSize / minMemoryMapAlignment it needs to construct a
// ForwarderAllocation with a source-visible staging pointer that preserves
// Vulkan's mapped-pointer alignment contract.
class MemoryTypeRegistry {
public:
    static MemoryTypeRegistry & instance();

    void record_device(VkDevice device, VkPhysicalDevice physical_device);
    void forget_device(VkDevice device);

    void record_memory_properties(VkPhysicalDevice physical_device,
                                  const VkPhysicalDeviceMemoryProperties & properties);
    void record_non_coherent_atom_size(VkPhysicalDevice physical_device,
                                       VkDeviceSize size);
    void record_min_memory_map_alignment(VkPhysicalDevice physical_device,
                                         std::size_t alignment);

    struct Resolved {
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        std::size_t           min_memory_map_alignment;
    };

    std::optional<Resolved> resolve(VkDevice device,
                                    std::uint32_t memory_type_index) const;

private:
    MemoryTypeRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<VkDevice, VkPhysicalDevice>                         device_to_physical_;
    std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> memory_properties_;
    std::unordered_map<VkPhysicalDevice, VkDeviceSize>                     non_coherent_atom_;
    std::unordered_map<VkPhysicalDevice, std::size_t>                      min_memory_map_alignment_;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 5: Add the implementation**

Create `src/vkfwd/ferry/core/memory_map/memory_type_registry.cpp`:

```cpp
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::memory_map {

MemoryTypeRegistry & MemoryTypeRegistry::instance() {
    static MemoryTypeRegistry s_instance;
    return s_instance;
}

void MemoryTypeRegistry::record_device(VkDevice device, VkPhysicalDevice physical_device) {
    if (device == VK_NULL_HANDLE || physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    device_to_physical_[device] = physical_device;
}

void MemoryTypeRegistry::forget_device(VkDevice device) {
    std::lock_guard lock(mutex_);
    device_to_physical_.erase(device);
}

void MemoryTypeRegistry::record_memory_properties(
    VkPhysicalDevice physical_device,
    const VkPhysicalDeviceMemoryProperties & properties) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    memory_properties_[physical_device] = properties;
}

void MemoryTypeRegistry::record_non_coherent_atom_size(
    VkPhysicalDevice physical_device, VkDeviceSize size) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    non_coherent_atom_[physical_device] = size;
}

void MemoryTypeRegistry::record_min_memory_map_alignment(
    VkPhysicalDevice physical_device, std::size_t alignment) {
    if (physical_device == VK_NULL_HANDLE) { return; }
    std::lock_guard lock(mutex_);
    min_memory_map_alignment_[physical_device] = alignment;
}

std::optional<MemoryTypeRegistry::Resolved>
MemoryTypeRegistry::resolve(VkDevice device, std::uint32_t memory_type_index) const {
    std::lock_guard lock(mutex_);

    const auto device_iter = device_to_physical_.find(device);
    if (device_iter == device_to_physical_.end()) { return std::nullopt; }
    const VkPhysicalDevice physical_device = device_iter->second;

    const auto props_iter = memory_properties_.find(physical_device);
    if (props_iter == memory_properties_.end()) { return std::nullopt; }
    const auto & props = props_iter->second;
    if (memory_type_index >= props.memoryTypeCount) { return std::nullopt; }

    const auto atom_iter = non_coherent_atom_.find(physical_device);
    if (atom_iter == non_coherent_atom_.end()) { return std::nullopt; }

    const auto map_alignment_iter = min_memory_map_alignment_.find(physical_device);
    if (map_alignment_iter == min_memory_map_alignment_.end()) { return std::nullopt; }

    return Resolved {
        .property_flags            = props.memoryTypes[memory_type_index].propertyFlags,
        .non_coherent_atom_size    = atom_iter->second,
        .min_memory_map_alignment  = map_alignment_iter->second,
    };
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 6: Add the new source to `src/vkfwd/ferry/core/CMakeLists.txt`**

In the `add_library(vkfwd_ferry_core STATIC ...)` source list, add (alphabetical insertion next to `memory_map/manager.cpp`):

```cmake
memory_map/memory_type_registry.cpp
```

- [ ] **Step 7: Run the test suite**

```bash
python3 dev/bin/cit.py
```

Expected: all tests pass, including the four new `MemoryTypeRegistry` tests.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add MemoryTypeRegistry forwarder-side cache

New core/memory_map/memory_type_registry.{hpp,cpp} caches
device->physical and physical->{memory properties, nonCoherentAtomSize}
maps, populated by hooks added in the next task. resolve() returns
nullopt when any precondition record is missing.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Add the four registry-feeding forwarder hooks plus integration test

**Files:**
- Create: `src/vkfwd/ferry/forwarder/hook/vkCreateDeviceForwarderHook.hpp`
- Create: `src/vkfwd/ferry/forwarder/hook/vkDestroyDeviceForwarderHook.hpp`
- Create: `src/vkfwd/ferry/forwarder/hook/vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp`
- Create: `src/vkfwd/ferry/forwarder/hook/vkGetPhysicalDevicePropertiesForwarderHook.hpp`
- Create: `src/vkfwd/ferry/forwarder/test/memory_type_registry_hooks_test.cpp`
- Modify: `src/vkfwd/ferry/forwarder/test/internal-test.cmake`

- [ ] **Step 1: Write the integration test**

Create `src/vkfwd/ferry/forwarder/test/memory_type_registry_hooks_test.cpp`:

```cpp
#include "support.hpp"

#include "generated/command/vkCreateDevice.hpp"
#include "generated/command/vkDestroyDevice.hpp"
#include "generated/command/vkGetPhysicalDeviceMemoryProperties.hpp"
#include "generated/command/vkGetPhysicalDeviceProperties.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using ::vkfwd::memory_map::MemoryTypeRegistry;

VkPhysicalDeviceMemoryProperties make_props() {
    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 1;
    props.memoryTypes[0]  = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    return props;
}

VkPhysicalDeviceProperties make_phys_dev_props(VkDeviceSize atom_size) {
    VkPhysicalDeviceProperties props {};
    props.limits.nonCoherentAtomSize = atom_size;
    props.limits.minMemoryMapAlignment = 4096;
    return props;
}

} // namespace

TEST_CASE("vkCreateDevice forwarder hook records device->physicalDevice mapping") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   physical_device = test_handle<VkPhysicalDevice>(0x310);
    auto   created_device  = test_handle<VkDevice>(0x410);
    registry.forget_device(created_device);

    install_pack_unpack_transport([&](CommandStream & request) {
        // Synthesize a success response with the test's device handle.
        using Command = ::vkfwd::generated::commands::vkCreateDevice::Command;
        const Range packet = first_command_range(request);
        auto        bytes  = command_view(request, packet);
        const Command::Parameters * params = nullptr;
        REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);

        CommandStream     response;
        Command::Response r {.return_value = VK_SUCCESS, .pDevice = params->pDevice};
        *params->pDevice = created_device;
        REQUIRE(Command::pack_response(response, r) == VK_SUCCESS);
        return response;
    });

    VkDeviceCreateInfo create_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice           device = VK_NULL_HANDLE;
    const VkResult     result =
        vkfwd::forwarder::generated::vkCreateDevice_entry(physical_device, &create_info, nullptr, &device);
    REQUIRE(result == VK_SUCCESS);
    CHECK(device == created_device);

    // We cannot resolve() yet without memory properties + atom size, but the
    // device entry must exist. Drive vkGetPhysicalDeviceMemoryProperties +
    // vkGetPhysicalDeviceProperties next and then resolve.
    registry.record_memory_properties(physical_device, make_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);
    const auto resolved = registry.resolve(created_device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    registry.forget_device(created_device);
}

TEST_CASE("vkGetPhysicalDeviceMemoryProperties forwarder hook records memory properties") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   physical_device = test_handle<VkPhysicalDevice>(0x311);
    auto   device          = test_handle<VkDevice>(0x411);

    registry.forget_device(device);
    registry.record_device(device, physical_device);
    registry.record_non_coherent_atom_size(physical_device, 32);
    registry.record_min_memory_map_alignment(physical_device, 4096);

    install_pack_unpack_transport([&](CommandStream & request) {
        using Command = ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command;
        const Range packet = first_command_range(request);
        auto        bytes  = command_view(request, packet);
        const Command::Parameters * params = nullptr;
        REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);

        CommandStream     response;
        *params->pMemoryProperties = make_props();
        Command::Response r {.pMemoryProperties = params->pMemoryProperties};
        REQUIRE(Command::pack_response(response, r) == VK_SUCCESS);
        return response;
    });

    VkPhysicalDeviceMemoryProperties props {};
    vkfwd::forwarder::generated::vkGetPhysicalDeviceMemoryProperties_entry(physical_device, &props);

    const auto resolved = registry.resolve(device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->property_flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);

    registry.forget_device(device);
}

TEST_CASE("vkGetPhysicalDeviceProperties forwarder hook records nonCoherentAtomSize") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   physical_device = test_handle<VkPhysicalDevice>(0x312);
    auto   device          = test_handle<VkDevice>(0x412);

    registry.forget_device(device);
    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_props());

    install_pack_unpack_transport([&](CommandStream & request) {
        using Command = ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command;
        const Range packet = first_command_range(request);
        auto        bytes  = command_view(request, packet);
        const Command::Parameters * params = nullptr;
        REQUIRE(Command::unpack_parameters(bytes, &params) == VK_SUCCESS);

        CommandStream     response;
        *params->pProperties = make_phys_dev_props(128);
        Command::Response r {.pProperties = params->pProperties};
        REQUIRE(Command::pack_response(response, r) == VK_SUCCESS);
        return response;
    });

    VkPhysicalDeviceProperties props {};
    vkfwd::forwarder::generated::vkGetPhysicalDeviceProperties_entry(physical_device, &props);

    const auto resolved = registry.resolve(device, 0);
    REQUIRE(resolved.has_value());
    CHECK(resolved->non_coherent_atom_size == 128);
    CHECK(resolved->min_memory_map_alignment == props.limits.minMemoryMapAlignment);

    registry.forget_device(device);
}

TEST_CASE("vkDestroyDevice forwarder hook removes the device entry") {
    auto & registry        = MemoryTypeRegistry::instance();
    auto   physical_device = test_handle<VkPhysicalDevice>(0x313);
    auto   device          = test_handle<VkDevice>(0x413);

    registry.record_device(device, physical_device);
    registry.record_memory_properties(physical_device, make_props());
    registry.record_non_coherent_atom_size(physical_device, 64);
    registry.record_min_memory_map_alignment(physical_device, 4096);
    REQUIRE(registry.resolve(device, 0).has_value());

    install_pack_unpack_transport([&](CommandStream & request) {
        return CommandStream {};
    });

    vkfwd::forwarder::generated::vkDestroyDevice_entry(device, nullptr);

    CHECK_FALSE(registry.resolve(device, 0).has_value());
}

} // namespace vkfwd::forwarder::test
```

- [ ] **Step 2: Add the new test source to `src/vkfwd/ferry/forwarder/test/internal-test.cmake`**

Append `memory_type_registry_hooks_test.cpp` to `VKFWD_INTERNAL_TEST_LOCAL_SOURCES`:

```cmake
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  getprocaddr_test.cpp
  vkCreateInstance_test.cpp
  vkDestroyInstance_test.cpp
  vkCreateDevice_test.cpp
  vkDestroyDevice_test.cpp
  vkAllocateFreeMemory_test.cpp
  vkMapMemory_test.cpp
  memory_type_registry_hooks_test.cpp)
```

- [ ] **Step 3: Verify the test fails**

```bash
python3 dev/bin/cit.py
```

Expected: the four new test cases fail because the hook headers do not exist yet — the generated entry points run with default `CommandHooks` and do not populate the registry.

- [ ] **Step 4: Add `vkCreateDeviceForwarderHook.hpp`**

Create `src/vkfwd/ferry/forwarder/hook/vkCreateDeviceForwarderHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkCreateDevice.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::CreateDevice> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(
        const ::vkfwd::generated::commands::vkCreateDevice::Command::Parameters & parameters,
        ::vkfwd::generated::commands::vkCreateDevice::Command::Response & response) {
        if (response.return_value != VK_SUCCESS) { return; }
        if (!parameters.pDevice || *parameters.pDevice == VK_NULL_HANDLE) { return; }

        // Allocate-time code resolves memoryTypeIndex against properties keyed
        // by VkPhysicalDevice. vkAllocateMemory only gives us VkDevice, so the
        // forwarder must remember which physical device produced each logical
        // device the moment vkCreateDevice succeeds.
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_device(
            *parameters.pDevice, parameters.physicalDevice);
    }
};

} // namespace vkfwd::forwarder::manual
```

- [ ] **Step 5: Add `vkDestroyDeviceForwarderHook.hpp`**

Create `src/vkfwd/ferry/forwarder/hook/vkDestroyDeviceForwarderHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkDestroyDevice.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::DestroyDevice> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_pack_enabled            = true;
    static constexpr bool after_response_unpack_enabled = false;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_pack(
        const ::vkfwd::generated::commands::vkDestroyDevice::Command::Parameters & parameters) {
        // Drop the device entry as soon as the command is accepted into the
        // request stream. A later vkAllocateMemory on the same handle must not
        // resolve through the doomed device.
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().forget_device(parameters.device);
    }

    template<class Parameters, class Response>
    static constexpr void after_response_unpack(const Parameters &, Response &) noexcept {}
};

} // namespace vkfwd::forwarder::manual
```

- [ ] **Step 6: Add `vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp`**

Create `src/vkfwd/ferry/forwarder/hook/vkGetPhysicalDeviceMemoryPropertiesForwarderHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkGetPhysicalDeviceMemoryProperties.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::GetPhysicalDeviceMemoryProperties> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(
        const ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command::Parameters & parameters,
        ::vkfwd::generated::commands::vkGetPhysicalDeviceMemoryProperties::Command::Response & /*response*/) {
        if (!parameters.pMemoryProperties) { return; }
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_memory_properties(
            parameters.physicalDevice, *parameters.pMemoryProperties);
    }
};

} // namespace vkfwd::forwarder::manual
```

- [ ] **Step 7: Add `vkGetPhysicalDevicePropertiesForwarderHook.hpp`**

Create `src/vkfwd/ferry/forwarder/hook/vkGetPhysicalDevicePropertiesForwarderHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkGetPhysicalDeviceProperties.hpp"
#include "generated/forwarder_hooks.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::GetPhysicalDeviceProperties> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(
        const ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command::Parameters & parameters,
        ::vkfwd::generated::commands::vkGetPhysicalDeviceProperties::Command::Response & /*response*/) {
        if (!parameters.pProperties) { return; }
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_non_coherent_atom_size(
            parameters.physicalDevice, parameters.pProperties->limits.nonCoherentAtomSize);
        ::vkfwd::memory_map::MemoryTypeRegistry::instance().record_min_memory_map_alignment(
            parameters.physicalDevice, parameters.pProperties->limits.minMemoryMapAlignment);
    }
};

} // namespace vkfwd::forwarder::manual
```

- [ ] **Step 8: Run the test suite**

```bash
python3 dev/bin/cit.py
```

Expected: all tests pass including the four new hooks-integration tests.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add forwarder hooks that feed MemoryTypeRegistry

Four new entry-point hooks populate the registry:
  vkCreateDevice                     -> record_device
  vkDestroyDevice                    -> forget_device
  vkGetPhysicalDeviceMemoryProperties -> record_memory_properties
  vkGetPhysicalDeviceProperties      -> record_non_coherent_atom_size
The integration test drives each through the existing pack/unpack test
transport.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Add `ForwarderAllocation` abstract base and empty placeholder subclasses

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/forwarder_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder_allocation.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder/coherent_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder/coherent_allocation.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

No tests in this task — the placeholder subclasses return `VK_ERROR_FEATURE_NOT_PRESENT` from every method, and per project guidance we do not test empty shell functions.

- [ ] **Step 1: Add the abstract base header**

Create `src/vkfwd/ferry/core/memory_map/forwarder_allocation.hpp`:

```cpp
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkfwd::memory_map {

// Per-VkDeviceMemory polymorphic handler. One concrete subclass is
// instantiated at vkAllocateMemory time and lives until vkFreeMemory.
// Subclasses implement the strategy-specific behavior (non-coherent vs
// coherent); the manager holds them by base pointer.
class ForwarderAllocation {
public:
    struct CreationInfo {
        VkDevice              device;
        VkDeviceMemory        memory;
        VkDeviceSize          allocation_size;
        std::uint32_t         memory_type_index;
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        // Required to satisfy Vulkan's *ppData - offset alignment contract
        // when phase 1 allocates source-side staging. Carried on every
        // allocation so subclasses do not have to re-resolve it via the
        // registry.
        std::size_t           min_memory_map_alignment;
    };

    virtual ~ForwarderAllocation() = default;

    virtual VkResult map(VkDeviceSize offset, VkDeviceSize size,
                         VkMemoryMapFlags flags, void ** ppData) = 0;
    virtual void     unmap() = 0;
    virtual VkResult flush(VkDeviceSize offset, VkDeviceSize size) = 0;
    virtual VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) = 0;

    const CreationInfo & info() const { return info_; }

protected:
    explicit ForwarderAllocation(const CreationInfo & info): info_(info) {}

private:
    CreationInfo info_;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 2: Add the base's translation unit**

Create `src/vkfwd/ferry/core/memory_map/forwarder_allocation.cpp`:

```cpp
#include "memory_map/forwarder_allocation.hpp"

// Translation unit exists so future virtual destructors / inline-defined
// methods have a single anchoring object file. The header has no out-of-line
// definitions today; this file intentionally has no content.

namespace vkfwd::memory_map {} // namespace vkfwd::memory_map
```

- [ ] **Step 3: Add `NonCoherentForwarderAllocation` placeholder header**

Create `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.hpp`:

```cpp
#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 1 fills these methods in with real N2 behavior
// (source-owned staging, no synced-range tracking — see
// doc/memory_map_management.md for the rejected-N3 rationale). Until then
// every method returns VK_ERROR_FEATURE_NOT_PRESENT so callers cannot
// mistake the placeholder for working mapped-memory support.
class NonCoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size,
                 VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 4: Add `NonCoherentForwarderAllocation` placeholder implementation**

Create `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.cpp`:

```cpp
#include "memory_map/forwarder/non_coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

VkResult NonCoherentForwarderAllocation::map(
    VkDeviceSize /*offset*/, VkDeviceSize /*size*/, VkMemoryMapFlags /*flags*/, void ** ppData) {
    if (ppData) { *ppData = nullptr; }
    VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map not yet implemented (phase 0)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

void NonCoherentForwarderAllocation::unmap() {}

VkResult NonCoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult NonCoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 5: Add `CoherentForwarderAllocation` placeholder header**

Create `src/vkfwd/ferry/core/memory_map/forwarder/coherent_allocation.hpp`:

```cpp
#pragma once

#include "memory_map/forwarder_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 3 (or whichever phase ships the chosen
// coherent strategy) fills these methods in. Until then every method
// returns VK_ERROR_FEATURE_NOT_PRESENT.
class CoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size,
                 VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 6: Add `CoherentForwarderAllocation` placeholder implementation**

Create `src/vkfwd/ferry/core/memory_map/forwarder/coherent_allocation.cpp`:

```cpp
#include "memory_map/forwarder/coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

VkResult CoherentForwarderAllocation::map(
    VkDeviceSize /*offset*/, VkDeviceSize /*size*/, VkMemoryMapFlags /*flags*/, void ** ppData) {
    if (ppData) { *ppData = nullptr; }
    VKFWD_LOG_ERROR("vkfwd: CoherentForwarderAllocation::map not yet implemented (phase 0)");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

void CoherentForwarderAllocation::unmap() {}

VkResult CoherentForwarderAllocation::flush(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult CoherentForwarderAllocation::invalidate(VkDeviceSize /*offset*/, VkDeviceSize /*size*/) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 7: Update `src/vkfwd/ferry/core/CMakeLists.txt`**

Add the three new `.cpp` files to the `vkfwd_ferry_core` source list:

```cmake
memory_map/forwarder_allocation.cpp
memory_map/forwarder/non_coherent_allocation.cpp
memory_map/forwarder/coherent_allocation.cpp
```

- [ ] **Step 8: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests still pass; no new behavior to test in this task.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add ForwarderAllocation base + placeholder subclasses

New abstract base in core/memory_map/forwarder_allocation.hpp.
NonCoherentForwarderAllocation and CoherentForwarderAllocation ship as
empty placeholders that return VK_ERROR_FEATURE_NOT_PRESENT; phase 1 and
phase 3 fill them in.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — Add `ForwarderAllocationFactory`

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/forwarder_allocation_factory.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/forwarder_allocation_factory.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

No tests yet — the factory only branches into the two placeholder subclasses, and a `dynamic_cast` test would just confirm the dispatch into empty code. Phase 1 adds the factory branching test when one of the subclasses gains real behavior.

- [ ] **Step 1: Add the header**

Create `src/vkfwd/ferry/core/memory_map/forwarder_allocation_factory.hpp`:

```cpp
#pragma once

#include "memory_map/forwarder_allocation.hpp"

#include <memory>

namespace vkfwd::memory_map {

class ForwarderAllocationFactory {
public:
    // Builds the concrete subclass that matches the memory type's property
    // flags. Returns nullptr for non-host-visible allocations — the manager
    // records nothing for them, so a later vkMapMemory on that handle fails
    // at the manager lookup rather than reaching this code.
    static std::unique_ptr<ForwarderAllocation>
        create(const ForwarderAllocation::CreationInfo & info);
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 2: Add the implementation**

Create `src/vkfwd/ferry/core/memory_map/forwarder_allocation_factory.cpp`:

```cpp
#include "memory_map/forwarder_allocation_factory.hpp"

#include "memory_map/forwarder/coherent_allocation.hpp"
#include "memory_map/forwarder/non_coherent_allocation.hpp"

namespace vkfwd::memory_map {

std::unique_ptr<ForwarderAllocation>
ForwarderAllocationFactory::create(const ForwarderAllocation::CreationInfo & info) {
    const bool host_visible =
        (info.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool host_coherent =
        (info.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;

    // Non-mappable allocations have no map-manager identity; the manager
    // skips recording. vkMapMemory on such a handle later fails at the
    // manager lookup; the receiver-side driver would reject it anyway.
    if (!host_visible) { return nullptr; }

    if (host_coherent) { return std::make_unique<CoherentForwarderAllocation>(info); }
    return std::make_unique<NonCoherentForwarderAllocation>(info);
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 3: Update `src/vkfwd/ferry/core/CMakeLists.txt`**

Add to the `vkfwd_ferry_core` source list:

```cmake
memory_map/forwarder_allocation_factory.cpp
```

- [ ] **Step 4: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests still pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add ForwarderAllocationFactory

Branches on VkMemoryPropertyFlags to pick the concrete subclass at
vkAllocateMemory time. Non-host-visible memory returns nullptr; the
manager (next task) treats nullptr as "do not record".

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Rewrite `MemoryMapForwarder`, update the allocate hook, prime the registry in the existing test

This is the integration step. The four pieces have to land together because they change one shared signature (`MemoryMapForwarder::record_allocation`).

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/manager.hpp`
- Modify: `src/vkfwd/ferry/core/memory_map/manager.cpp`
- Modify: `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp`
- Modify: `src/vkfwd/ferry/forwarder/test/vkAllocateFreeMemory_test.cpp`

- [ ] **Step 1: Update `src/vkfwd/ferry/core/memory_map/manager.hpp`**

Replace the `MemoryMapForwarder` class body (keeping the surrounding namespace, the `MemoryMapReceiver` class, and `kMemoryMapManagerRevision` unchanged) with:

```cpp
class MemoryMapForwarder {
public:
    static MemoryMapForwarder & instance();

    void record_allocation(VkDevice device,
                           VkDeviceMemory memory,
                           VkMemoryPropertyFlags property_flags,
                           std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size,
                           VkDeviceSize non_coherent_atom_size,
                           std::size_t min_memory_map_alignment);

    void forget_allocation(VkDeviceMemory memory);

    VkResult custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory,
                                      VkDeviceSize offset, VkDeviceSize size,
                                      VkMemoryMapFlags flags, void ** ppData);

    void custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory);

    // Phase 2 will wire vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges
    // entry points into these. Phase 0 has no callers; the surface is locked now
    // so phase 2 only adds plumbing.
    VkResult flush_ranges(VkDevice device,
                          std::uint32_t range_count,
                          const VkMappedMemoryRange * ranges);
    VkResult invalidate_ranges(VkDevice device,
                               std::uint32_t range_count,
                               const VkMappedMemoryRange * ranges);

    VkDeviceSize test_get_allocation_size(VkDeviceMemory memory) const;

private:
    MemoryMapForwarder();
    ~MemoryMapForwarder();

    class Impl;
    Impl * impl_ = nullptr;
};
```

Add `#include <cstdint>` if it is not already present.

- [ ] **Step 2: Rewrite `src/vkfwd/ferry/core/memory_map/manager.cpp`**

Replace the `MemoryMapForwarder::Impl` class and its facade thunks with the per-allocation dispatch. Leave `MemoryMapReceiver` unchanged in this task; the receiver gets rewritten in task 9.

```cpp
#include "memory_map/manager.hpp"

#include "memory_map/forwarder_allocation.hpp"
#include "memory_map/forwarder_allocation_factory.hpp"
#include "logging.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace vkfwd {

class MemoryMapForwarder::Impl {
public:
    void record_allocation(VkDevice device,
                           VkDeviceMemory memory,
                           VkMemoryPropertyFlags property_flags,
                           std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size,
                           VkDeviceSize non_coherent_atom_size,
                           std::size_t min_memory_map_alignment) {
        if (memory == VK_NULL_HANDLE) { return; }

        auto allocation = ::vkfwd::memory_map::ForwarderAllocationFactory::create({
            .device                  = device,
            .memory                  = memory,
            .allocation_size         = allocation_size,
            .memory_type_index       = memory_type_index,
            .property_flags          = property_flags,
            .non_coherent_atom_size  = non_coherent_atom_size,
            .min_memory_map_alignment = min_memory_map_alignment,
        });
        if (!allocation) { return; }

        std::lock_guard lock(mutex);
        allocations[memory] = std::move(allocation);
    }

    void forget_allocation(VkDeviceMemory memory) {
        if (memory == VK_NULL_HANDLE) { return; }
        std::lock_guard lock(mutex);
        allocations.erase(memory);
    }

    VkResult custom_vkMapMemory_entry(VkDevice /*device*/, VkDeviceMemory memory,
                                      VkDeviceSize offset, VkDeviceSize size,
                                      VkMemoryMapFlags flags, void ** ppData) {
        ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
        {
            std::lock_guard lock(mutex);
            const auto      found = allocations.find(memory);
            if (found == allocations.end()) {
                // Vulkan-app-visible error preserved from the prior placeholder.
                // Phase 1's NonCoherentForwarderAllocation also returns this
                // until staging is real.
                if (ppData) { *ppData = nullptr; }
                VKFWD_LOG_ERROR(
                    "vkfwd: MemoryMapForwarder::custom_vkMapMemory_entry called for an unrecorded VkDeviceMemory");
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            allocation = found->second.get();
        }
        return allocation->map(offset, size, flags, ppData);
    }

    void custom_vkUnmapMemory_entry(VkDevice /*device*/, VkDeviceMemory memory) {
        ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
        {
            std::lock_guard lock(mutex);
            const auto      found = allocations.find(memory);
            if (found == allocations.end()) { return; }
            allocation = found->second.get();
        }
        allocation->unmap();
    }

    VkResult flush_ranges(VkDevice /*device*/, std::uint32_t range_count,
                          const VkMappedMemoryRange * ranges) {
        if (range_count == 0 || ranges == nullptr) { return VK_SUCCESS; }
        VkResult aggregated = VK_SUCCESS;
        for (std::uint32_t i = 0; i < range_count; ++i) {
            const auto & range = ranges[i];
            ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
            {
                std::lock_guard lock(mutex);
                const auto      found = allocations.find(range.memory);
                if (found == allocations.end()) {
                    aggregated = VK_ERROR_FEATURE_NOT_PRESENT;
                    continue;
                }
                allocation = found->second.get();
            }
            const VkResult r = allocation->flush(range.offset, range.size);
            if (r != VK_SUCCESS && aggregated == VK_SUCCESS) { aggregated = r; }
        }
        return aggregated;
    }

    VkResult invalidate_ranges(VkDevice /*device*/, std::uint32_t range_count,
                               const VkMappedMemoryRange * ranges) {
        if (range_count == 0 || ranges == nullptr) { return VK_SUCCESS; }
        VkResult aggregated = VK_SUCCESS;
        for (std::uint32_t i = 0; i < range_count; ++i) {
            const auto & range = ranges[i];
            ::vkfwd::memory_map::ForwarderAllocation * allocation = nullptr;
            {
                std::lock_guard lock(mutex);
                const auto      found = allocations.find(range.memory);
                if (found == allocations.end()) {
                    aggregated = VK_ERROR_FEATURE_NOT_PRESENT;
                    continue;
                }
                allocation = found->second.get();
            }
            const VkResult r = allocation->invalidate(range.offset, range.size);
            if (r != VK_SUCCESS && aggregated == VK_SUCCESS) { aggregated = r; }
        }
        return aggregated;
    }

    VkDeviceSize test_get_allocation_size(VkDeviceMemory memory) const {
        std::lock_guard lock(mutex);
        const auto      found = allocations.find(memory);
        if (found == allocations.end()) { return 0; }
        return found->second->info().allocation_size;
    }

    mutable std::mutex mutex;
    std::unordered_map<VkDeviceMemory,
                       std::unique_ptr<::vkfwd::memory_map::ForwarderAllocation>> allocations;
};

MemoryMapForwarder::MemoryMapForwarder(): impl_(new Impl()) {}
MemoryMapForwarder::~MemoryMapForwarder() { delete impl_; }

MemoryMapForwarder & MemoryMapForwarder::instance() {
    static MemoryMapForwarder s_instance;
    return s_instance;
}

void MemoryMapForwarder::record_allocation(VkDevice device,
                                           VkDeviceMemory memory,
                                           VkMemoryPropertyFlags property_flags,
                                           std::uint32_t memory_type_index,
                                           VkDeviceSize allocation_size,
                                           VkDeviceSize non_coherent_atom_size,
                                           std::size_t min_memory_map_alignment) {
    impl_->record_allocation(device, memory, property_flags, memory_type_index,
                              allocation_size, non_coherent_atom_size,
                              min_memory_map_alignment);
}

void MemoryMapForwarder::forget_allocation(VkDeviceMemory memory) {
    impl_->forget_allocation(memory);
}

VkResult MemoryMapForwarder::custom_vkMapMemory_entry(VkDevice device, VkDeviceMemory memory,
                                                      VkDeviceSize offset, VkDeviceSize size,
                                                      VkMemoryMapFlags flags, void ** ppData) {
    return impl_->custom_vkMapMemory_entry(device, memory, offset, size, flags, ppData);
}

void MemoryMapForwarder::custom_vkUnmapMemory_entry(VkDevice device, VkDeviceMemory memory) {
    impl_->custom_vkUnmapMemory_entry(device, memory);
}

VkResult MemoryMapForwarder::flush_ranges(VkDevice device,
                                          std::uint32_t range_count,
                                          const VkMappedMemoryRange * ranges) {
    return impl_->flush_ranges(device, range_count, ranges);
}

VkResult MemoryMapForwarder::invalidate_ranges(VkDevice device,
                                               std::uint32_t range_count,
                                               const VkMappedMemoryRange * ranges) {
    return impl_->invalidate_ranges(device, range_count, ranges);
}

VkDeviceSize MemoryMapForwarder::test_get_allocation_size(VkDeviceMemory memory) const {
    return impl_->test_get_allocation_size(memory);
}

// MemoryMapReceiver implementation lives below this point unchanged from the
// pre-task-6 contents. Do not touch it in this task — task 9 rewrites it.

} // namespace vkfwd
```

Keep the original `MemoryMapReceiver` impl (lines starting at `class MemoryMapReceiver::Impl` through the end of the namespace) verbatim from the post-task-1 file.

- [ ] **Step 3: Update `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp`**

Replace the file contents with:

```cpp
#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_hooks.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_type_registry.hpp"

namespace vkfwd::forwarder::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::AllocateMemory> {
    static constexpr bool before_pack_enabled           = false;
    static constexpr bool after_response_unpack_enabled = true;

    template<class... Args>
    static constexpr void before_pack(Args &...) noexcept {}

    static void after_response_unpack(
        const ::vkfwd::generated::commands::vkAllocateMemory::Command::Parameters & parameters,
        ::vkfwd::generated::commands::vkAllocateMemory::Command::Response & response) {
        if (response.return_value != VK_SUCCESS || !parameters.pAllocateInfo
            || !response.pMemory || *response.pMemory == VK_NULL_HANDLE) {
            return;
        }

        const auto resolved = ::vkfwd::memory_map::MemoryTypeRegistry::instance().resolve(
            parameters.device, parameters.pAllocateInfo->memoryTypeIndex);
        if (!resolved) {
            // Phase 0 has only the opportunistic cache. Vulkan does not require
            // the app to call property queries before allocation, so this is a
            // vkfwd classification miss rather than an app error. Phase 1 adds
            // manual::CommandId::QueryPhysicalDeviceMemoryInfo as the fallback.
            VKFWD_LOG_ERROR(
                "vkfwd: memory_type_registry has no entry for device={} memoryTypeIndex={}; "
                "vkAllocateMemory tracked record skipped",
                static_cast<void *>(parameters.device),
                parameters.pAllocateInfo->memoryTypeIndex);
            return;
        }

        ::vkfwd::MemoryMapForwarder::instance().record_allocation(
            parameters.device,
            *response.pMemory,
            resolved->property_flags,
            parameters.pAllocateInfo->memoryTypeIndex,
            parameters.pAllocateInfo->allocationSize,
            resolved->non_coherent_atom_size,
            resolved->min_memory_map_alignment);
    }
};

} // namespace vkfwd::forwarder::manual
```

- [ ] **Step 4: Update `src/vkfwd/ferry/forwarder/test/vkAllocateFreeMemory_test.cpp`**

Replace the file contents with:

```cpp
#include "support.hpp"

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/forwarder_entrypoints.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/memory_type_registry.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::forwarder::test {
namespace {

using AllocateCommand = ::vkfwd::generated::commands::vkAllocateMemory::Command;

constexpr VkDeviceSize kAllocationSize = 64 * 1024;

struct Scenario {
    VkPhysicalDevice     physical_device = test_handle<VkPhysicalDevice>(0x401);
    VkDevice             device          = test_handle<VkDevice>(0x501);
    VkDeviceMemory       receiver_memory = test_handle<VkDeviceMemory>(0x601);
    VkMemoryAllocateInfo allocate_info {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext           = nullptr,
        .allocationSize  = kAllocationSize,
        .memoryTypeIndex = 0,
    };
};

Scenario & scenario() {
    static Scenario value;
    return value;
}

void prime_registry(const Scenario & s) {
    auto & registry = ::vkfwd::memory_map::MemoryTypeRegistry::instance();
    registry.forget_device(s.device);
    registry.record_device(s.device, s.physical_device);

    VkPhysicalDeviceMemoryProperties props {};
    props.memoryTypeCount = 1;
    props.memoryTypes[0]  = {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0};
    registry.record_memory_properties(s.physical_device, props);

    registry.record_non_coherent_atom_size(s.physical_device, 64);
    registry.record_min_memory_map_alignment(s.physical_device, 4096);
}

CommandStream handle_allocate_flush(CommandStream & request_stream) {
    auto &       expected = scenario();
    const Range  packet   = first_command_range(request_stream);
    auto         bytes    = command_view(request_stream, packet);
    const auto * actual   = static_cast<const AllocateCommand::Parameters *>(nullptr);
    REQUIRE(AllocateCommand::unpack_parameters(bytes, &actual) == VK_SUCCESS);
    REQUIRE(actual != nullptr);
    CHECK(actual->device == expected.device);
    REQUIRE(actual->pAllocateInfo != nullptr);
    CHECK(actual->pAllocateInfo->allocationSize == expected.allocate_info.allocationSize);

    CommandStream             response_stream;
    AllocateCommand::Response response {.return_value = VK_SUCCESS, .pMemory = &expected.receiver_memory};
    REQUIRE(AllocateCommand::pack_response(response_stream, response) == VK_SUCCESS);
    return response_stream;
}

} // namespace

TEST_CASE("vkAllocateMemory and vkFreeMemory update memory map allocation records") {
    auto & manager = ::vkfwd::MemoryMapForwarder::instance();
    auto & expected = scenario();
    manager.forget_allocation(expected.receiver_memory);
    prime_registry(expected);
    install_pack_unpack_transport(handle_allocate_flush);

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkfwd::forwarder::generated::vkAllocateMemory_entry(
        expected.device, &expected.allocate_info, nullptr, &memory);

    REQUIRE(result == VK_SUCCESS);
    CHECK(memory == expected.receiver_memory);
    VkDeviceSize recorded_size = manager.test_get_allocation_size(memory);
    CHECK(recorded_size == expected.allocate_info.allocationSize);

    vkfwd::forwarder::generated::vkFreeMemory_entry(expected.device, memory, nullptr);

    CHECK(manager.test_get_allocation_size(memory) == 0);
}

} // namespace vkfwd::forwarder::test
```

- [ ] **Step 5: Run the test suite**

```bash
python3 dev/bin/cit.py
```

Expected: all tests pass. The updated `vkAllocateFreeMemory_test` now primes the registry; the new manager dispatch creates a `NonCoherentForwarderAllocation` for the host-visible memory type.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP convert MemoryMapForwarder to per-allocation polymorphic dispatch

MemoryMapForwarder::Impl::allocations now holds
unordered_map<VkDeviceMemory, unique_ptr<ForwarderAllocation>>.
record_allocation grows arguments for property flags, memory type index,
and nonCoherentAtomSize; the vkAllocateMemory hook resolves them via
MemoryTypeRegistry. New flush_ranges / invalidate_ranges methods are in
place but uncalled until phase 2 entry points land.

vkAllocateFreeMemory_test now primes the registry before driving
vkAllocateMemory_entry so the hook can resolve property flags.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 7 — Add `ReceiverAllocation` abstract base and empty placeholder subclasses

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/receiver_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver_allocation.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver/coherent_allocation.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver/coherent_allocation.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

- [ ] **Step 1: Add the abstract base header**

Create `src/vkfwd/ferry/core/memory_map/receiver_allocation.hpp`:

```cpp
#pragma once

#include "command_stream.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vkfwd::receiver { struct ReplayContext; }

namespace vkfwd::memory_map {

// Per-VkDeviceMemory polymorphic handler on the receiver side. One
// concrete subclass per allocation, owned by MemoryMapReceiver's
// per-handle map. The receiver mapped pointer (if any) stays private to
// the subclass and must never escape to source-process state.
class ReceiverAllocation {
public:
    struct CreationInfo {
        // Source-visible VkDevice (also the per-context dispatch key); the
        // receiver-native VkDevice that real Vulkan calls need is obtained
        // via ReplayContext's source→receiver handle map at call sites,
        // matching how every other receiver endpoint performs translation.
        VkDevice              device;
        // Source-visible VkDeviceMemory; this same value is the
        // MemoryMapReceiver per-handle map key and matches the handle that
        // arrives in wire payloads. The receiver-native VkDeviceMemory is
        // also obtained via ReplayContext's handle map at call sites. The
        // handle-map dependency is a phase-1 prerequisite (see "What's
        // Deferred" in doc/memory_map_management.md).
        VkDeviceMemory        memory;
        VkDeviceSize          allocation_size;
        std::uint32_t         memory_type_index;
        VkMemoryPropertyFlags property_flags;
        VkDeviceSize          non_coherent_atom_size;
        // Required to satisfy Vulkan's *ppData - offset alignment contract
        // mirror on the receiver side, and carried on every allocation so
        // future receiver-side alignment validation does not require
        // widening this struct later.
        std::size_t           min_memory_map_alignment;
    };

    virtual ~ReceiverAllocation() = default;

    virtual bool map_endpoint(const CommandStream & request_stream,
                              const Range & request_range,
                              CommandStream & response_stream,
                              ::vkfwd::receiver::ReplayContext & replay_context) = 0;
    virtual bool unmap_endpoint(const CommandStream & request_stream,
                                const Range & request_range,
                                CommandStream & response_stream,
                                ::vkfwd::receiver::ReplayContext & replay_context) = 0;
    virtual bool flush_endpoint(const CommandStream & request_stream,
                                const Range & request_range,
                                CommandStream & response_stream,
                                ::vkfwd::receiver::ReplayContext & replay_context) = 0;
    virtual bool invalidate_endpoint(const CommandStream & request_stream,
                                     const Range & request_range,
                                     CommandStream & response_stream,
                                     ::vkfwd::receiver::ReplayContext & replay_context) = 0;

    const CreationInfo & info() const { return info_; }

protected:
    explicit ReceiverAllocation(const CreationInfo & info): info_(info) {}

private:
    CreationInfo info_;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 2: Add the base's translation unit**

Create `src/vkfwd/ferry/core/memory_map/receiver_allocation.cpp`:

```cpp
#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {} // namespace vkfwd::memory_map
```

- [ ] **Step 3: Add `NonCoherentReceiverAllocation` placeholder header**

Create `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.hpp`:

```cpp
#pragma once

#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder. Phase 1 fills these methods in with real receiver
// staging behavior. Until then every method returns false so the receiver
// endpoint reports a replay failure rather than silently doing nothing.
class NonCoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &,
                      ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &,
                        ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &,
                        ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &,
                             ::vkfwd::receiver::ReplayContext &) override;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 4: Add `NonCoherentReceiverAllocation` placeholder implementation**

Create `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.cpp`:

```cpp
#include "memory_map/receiver/non_coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

bool NonCoherentReceiverAllocation::map_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR("vkfwd: NonCoherentReceiverAllocation::map_endpoint not yet implemented (phase 0)");
    return false;
}

bool NonCoherentReceiverAllocation::unmap_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

bool NonCoherentReceiverAllocation::flush_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

bool NonCoherentReceiverAllocation::invalidate_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 5: Add `CoherentReceiverAllocation` placeholder header**

Create `src/vkfwd/ferry/core/memory_map/receiver/coherent_allocation.hpp`:

```cpp
#pragma once

#include "memory_map/receiver_allocation.hpp"

namespace vkfwd::memory_map {

// Phase-0 placeholder; phase 3 (or whichever phase ships the coherent
// strategy) fills the methods. Until then every method returns false.
class CoherentReceiverAllocation final : public ReceiverAllocation {
public:
    using ReceiverAllocation::ReceiverAllocation;

    bool map_endpoint(const CommandStream &, const Range &, CommandStream &,
                      ::vkfwd::receiver::ReplayContext &) override;
    bool unmap_endpoint(const CommandStream &, const Range &, CommandStream &,
                        ::vkfwd::receiver::ReplayContext &) override;
    bool flush_endpoint(const CommandStream &, const Range &, CommandStream &,
                        ::vkfwd::receiver::ReplayContext &) override;
    bool invalidate_endpoint(const CommandStream &, const Range &, CommandStream &,
                             ::vkfwd::receiver::ReplayContext &) override;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 6: Add `CoherentReceiverAllocation` placeholder implementation**

Create `src/vkfwd/ferry/core/memory_map/receiver/coherent_allocation.cpp`:

```cpp
#include "memory_map/receiver/coherent_allocation.hpp"

#include "logging.hpp"

namespace vkfwd::memory_map {

bool CoherentReceiverAllocation::map_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    VKFWD_LOG_ERROR("vkfwd: CoherentReceiverAllocation::map_endpoint not yet implemented (phase 0)");
    return false;
}

bool CoherentReceiverAllocation::unmap_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

bool CoherentReceiverAllocation::flush_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

bool CoherentReceiverAllocation::invalidate_endpoint(
    const CommandStream &, const Range &, CommandStream &, ::vkfwd::receiver::ReplayContext &) {
    return false;
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 7: Update `src/vkfwd/ferry/core/CMakeLists.txt`**

Add the three new `.cpp` files to the `vkfwd_ferry_core` source list:

```cmake
memory_map/receiver_allocation.cpp
memory_map/receiver/non_coherent_allocation.cpp
memory_map/receiver/coherent_allocation.cpp
```

- [ ] **Step 8: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests still pass.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add ReceiverAllocation base + placeholder subclasses

Mirror of the forwarder-side hierarchy. The per-handle map inside
MemoryMapReceiver stays empty in phase 0 (no caller populates it until
phase 1's manual command dispatch and handle-map state land); these classes exist now so
phase 1 only has to fill bodies in, not reshape the type system.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 8 — Add `ReceiverAllocationFactory`

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/receiver_allocation_factory.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/receiver_allocation_factory.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

- [ ] **Step 1: Add the header**

Create `src/vkfwd/ferry/core/memory_map/receiver_allocation_factory.hpp`:

```cpp
#pragma once

#include "memory_map/receiver_allocation.hpp"

#include <memory>

namespace vkfwd::memory_map {

class ReceiverAllocationFactory {
public:
    // Phase 0 has no callers; the branching is identical to the forwarder
    // factory. Phase 1 wires this into the receiver-side state used by
    // manual::CommandId::MemoryMap / MemoryUnmap.
    static std::unique_ptr<ReceiverAllocation>
        create(const ReceiverAllocation::CreationInfo & info);
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 2: Add the implementation**

Create `src/vkfwd/ferry/core/memory_map/receiver_allocation_factory.cpp`:

```cpp
#include "memory_map/receiver_allocation_factory.hpp"

#include "memory_map/receiver/coherent_allocation.hpp"
#include "memory_map/receiver/non_coherent_allocation.hpp"

namespace vkfwd::memory_map {

std::unique_ptr<ReceiverAllocation>
ReceiverAllocationFactory::create(const ReceiverAllocation::CreationInfo & info) {
    const bool host_visible =
        (info.property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    const bool host_coherent =
        (info.property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (!host_visible) { return nullptr; }
    if (host_coherent) { return std::make_unique<CoherentReceiverAllocation>(info); }
    return std::make_unique<NonCoherentReceiverAllocation>(info);
}

} // namespace vkfwd::memory_map
```

- [ ] **Step 3: Update `src/vkfwd/ferry/core/CMakeLists.txt`**

Add to the `vkfwd_ferry_core` source list:

```cmake
memory_map/receiver_allocation_factory.cpp
```

- [ ] **Step 4: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests still pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP add ReceiverAllocationFactory

Mirror of ForwarderAllocationFactory. No callers in phase 0; phase 1 wires
this into the receiver vkAllocateMemory endpoint once the receiver hook
framework lands.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 9 — Rewrite `MemoryMapReceiver` over the new shape

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/manager.hpp`
- Modify: `src/vkfwd/ferry/core/memory_map/manager.cpp`

The receiver's per-handle map stays empty for the whole of phase 0 (no caller populates it). The public receiver facade exposes custom endpoint names for vkfwd-owned manual command ids, but the methods return `false` until phase 1 adds manual receiver dispatch and real memory-map payload unpacking.

- [ ] **Step 1: Update `MemoryMapReceiver`'s class declaration in `manager.hpp`**

Replace the existing `MemoryMapReceiver` class body (within the `vkfwd::` namespace) with:

```cpp
class MemoryMapReceiver {
public:
    MemoryMapReceiver();
    ~MemoryMapReceiver();

    // Phase 0 leaves these uncalled by generated standard Vulkan endpoint code.
    // Phase 1 dispatches manual::CommandId::MemoryMap / MemoryUnmap chunks to
    // the custom endpoint methods below.
    void record_allocation(VkDevice device,
                           VkDeviceMemory memory,
                           VkMemoryPropertyFlags property_flags,
                           std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size,
                           VkDeviceSize non_coherent_atom_size,
                           std::size_t min_memory_map_alignment);
    void forget_allocation(VkDeviceMemory memory);

    bool custom_vkMapMemory_endpoint(const CommandStream & request_stream,
                                     const Range & request_range,
                                     CommandStream & response_stream);
    bool custom_vkUnmapMemory_endpoint(const CommandStream & request_stream,
                                       const Range & request_range,
                                       CommandStream & response_stream);

private:
    class Impl;
    Impl * impl_ = nullptr;
};
```

The custom names are deliberate: these methods handle vkfwd's manual
`MemoryMap` / `MemoryUnmap` command ids, not standard generated Vulkan
`CommandId::MapMemory` / `CommandId::UnmapMemory` chunks.

- [ ] **Step 2: Rewrite the `MemoryMapReceiver` portion of `manager.cpp`**

Replace the `MemoryMapReceiver::Impl` class and the facade thunks with the version below. The shape is deliberately minimal: per-handle storage map and the `record_allocation` / `forget_allocation` plumbing exist, while the custom endpoints return `false` until phase 1 adds the manual command payload unpacking and receiver dispatch.

```cpp
#include "memory_map/receiver_allocation.hpp"
#include "memory_map/receiver_allocation_factory.hpp"

namespace vkfwd {

class MemoryMapReceiver::Impl {
public:
    void record_allocation(VkDevice device,
                           VkDeviceMemory memory,
                           VkMemoryPropertyFlags property_flags,
                           std::uint32_t memory_type_index,
                           VkDeviceSize allocation_size,
                           VkDeviceSize non_coherent_atom_size,
                           std::size_t min_memory_map_alignment) {
        if (memory == VK_NULL_HANDLE) { return; }
        auto allocation = ::vkfwd::memory_map::ReceiverAllocationFactory::create({
            .device                  = device,
            .memory                  = memory,
            .allocation_size         = allocation_size,
            .memory_type_index       = memory_type_index,
            .property_flags          = property_flags,
            .non_coherent_atom_size  = non_coherent_atom_size,
            .min_memory_map_alignment = min_memory_map_alignment,
        });
        if (!allocation) { return; }
        allocations[memory] = std::move(allocation);
    }

    void forget_allocation(VkDeviceMemory memory) {
        if (memory == VK_NULL_HANDLE) { return; }
        allocations.erase(memory);
    }

    std::unordered_map<VkDeviceMemory,
                       std::unique_ptr<::vkfwd::memory_map::ReceiverAllocation>> allocations;
};

MemoryMapReceiver::MemoryMapReceiver(): impl_(new Impl()) {}
MemoryMapReceiver::~MemoryMapReceiver() { delete impl_; }

void MemoryMapReceiver::record_allocation(VkDevice device,
                                          VkDeviceMemory memory,
                                          VkMemoryPropertyFlags property_flags,
                                          std::uint32_t memory_type_index,
                                          VkDeviceSize allocation_size,
                                          VkDeviceSize non_coherent_atom_size,
                                          std::size_t min_memory_map_alignment) {
    impl_->record_allocation(device, memory, property_flags, memory_type_index,
                              allocation_size, non_coherent_atom_size,
                              min_memory_map_alignment);
}

void MemoryMapReceiver::forget_allocation(VkDeviceMemory memory) {
    impl_->forget_allocation(memory);
}

bool MemoryMapReceiver::custom_vkMapMemory_endpoint(const CommandStream & /*request_stream*/,
                                                    const Range & /*request_range*/,
                                                    CommandStream & /*response_stream*/) {
    // Phase 0 placeholder behavior preserved. Phase 1 unpacks
    // manual::CommandId::MemoryMap and delegates into ReceiverAllocation.
    return false;
}

bool MemoryMapReceiver::custom_vkUnmapMemory_endpoint(const CommandStream & /*request_stream*/,
                                                      const Range & /*request_range*/,
                                                      CommandStream & /*response_stream*/) {
    return false;
}

} // namespace vkfwd
```

Note: the per-handle map exists and `record_allocation` / `forget_allocation` work, but nothing in phase-0 code calls them. Phase 1 adds manual receiver dispatch for `vkfwd::manual::CommandId` and the handle/allocation state needed by the custom memory-map protocol.

- [ ] **Step 3: Verify the build is green**

```bash
python3 dev/bin/cit.py
```

Expected: all tests still pass. The existing `vkMapMemory_test` still passes because `MemoryMapForwarder::custom_vkMapMemory_entry` returns `VK_ERROR_FEATURE_NOT_PRESENT` for unrecorded handles (the test never allocates first).

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
WIP convert MemoryMapReceiver to per-allocation polymorphic dispatch

Mirror of task 6 on the receiver side. Adds record_allocation /
forget_allocation methods and per-handle map of unique_ptr<ReceiverAllocation>.
The public custom_vkMapMemory_endpoint / custom_vkUnmapMemory_endpoint
wrappers remain no-ops returning false; phase 1 wires manual command dispatch
to these methods and fills in ReceiverAllocation delegation.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 10 — Final regression run

**Files:** none modified.

- [ ] **Step 1: Clean build**

```bash
dev/bin/build.py c
dev/bin/build.py d
```

Expected: clean reconfigure plus full debug rebuild succeed.

- [ ] **Step 2: Run the full test suite**

```bash
python3 dev/bin/cit.py
```

Expected: 977 (or more, given the new tests added in tasks 2 and 3) assertions across 83 (or more) test cases all pass. Format-check should pass.

- [ ] **Step 3: Eyeball the diff against `main`**

```bash
git log --oneline "$(git merge-base HEAD origin/main)"..HEAD
git diff --stat "$(git merge-base HEAD origin/main)"..HEAD
```

Expected: WIP commits for the plan tasks. Diff includes the new
`core/memory_map/` tree, `core/custom_command.hpp`, the four new forwarder hook
headers, the registry integration test, and the modified
`vkAllocateFreeMemory_test.cpp`.

- [ ] **Step 4: Confirm phase-0 acceptance**

Manually walk through the spec's "Phase 0 — Skeleton Framework" section in `doc/memory_map_management.md` and tick each item against the committed diff. No spec item should be missing or behaviorally divergent. Note any deviation in a follow-up issue rather than silently changing the spec.

No additional commit in this task.

---

## Out of scope for phase 0

These remain to be addressed in the dependent / parallel work, not in this plan:

- Adding `vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` to the generator's `TARGET_COMMANDS`. Lands with phase 2.
- Receiver dispatch for `vkfwd::manual::CommandId` and the custom
  memory-map payload handlers. Lands phase 1.
- Real per-strategy behavior in `NonCoherent*` / `Coherent*` subclasses. Lands phase 1 / phase 3 respectively.
- `manual::CommandId::QueryPhysicalDeviceMemoryInfo` fallback query for apps
  that allocate without first priming `MemoryTypeRegistry`. Lands phase 1.
- Generator changes that enable full manual public `vkMapMemory` /
  `vkUnmapMemory` forwarder delegation via
  `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS`. Lands phase 1.
