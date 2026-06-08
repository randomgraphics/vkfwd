# Memory Map Phases 1+2+3a Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship fully Vulkan-spec-compliant non-coherent mapped memory (Phase 1 N2 map/unmap + Phase 2 flush/invalidate) and the C2.1 sub-stage of coherent mapped memory (map/unmap bracketed copies, covering the common one-shot upload/download patterns).

**Architecture:** Real source-side VM reserve+commit staging on the forwarder side, manual `vkfwd::manual::CommandId` wire protocol with the receiver translating source→receiver `VkDeviceMemory` handles before calling the real driver, and per-strategy implementations (`NonCoherentForwarderAllocation`/`NonCoherentReceiverAllocation` for N2; `CoherentForwarderAllocation`/`CoherentReceiverAllocation` for C2.1) inside the polymorphic framework that Phase 0 already established.

**Tech Stack:** C++20, CMake, Catch2, Vulkan, spdlog, rapid-vulkan (loopback runtime for end-to-end tests), POSIX/Win32 VM primitives.

**Reference spec:** `doc/memory_map_management.md` — read the "Phase 1+ — Wire Protocol", "Source-Side Staging: VM Reserve + Commit", and "Phase 1+ — Algorithm Sketches" sections before starting Phase 1 tasks, and the "Coherent — implementation alternatives" / "C2.1" sections before starting Phase 3a.

**Validation command:** `python3 dev/bin/cit.py --build-dir build/macos.clang.debug` on macOS, `--build-dir build/linux.gcc.debug` on Linux. Run from repo root.

**Test policy:**
- Pure CPU-side pack/unpack tests for every new wire-format struct (same pattern as Phase 0).
- Forwarder-side tests use `install_pack_unpack_transport(...)` to synthesize fake receiver responses (same as the existing `vkAllocateFreeMemory_test.cpp` and `memory_type_registry_hooks_test.cpp`).
- Receiver-side tests construct wire chunks directly and assert on the receiver-side state changes; they may call into a fake `DispatchTable` whose function pointers redirect to test stubs (no real driver required for the unit slice).
- One **end-to-end loopback test** per phase under `src/vkfwd/ferry/test/` using `loopback_runtime.hpp` + rapid-vulkan. These exercise the full forwarder→transport→receiver path against a real Vulkan driver (the same way `create-instance-test.cpp` does for vkCreateInstance today).

---

## Phase boundaries and stop points

This plan ships three independent phases. Each phase ends in a fully working, Vulkan-spec-compliant state with passing tests; you may pause execution between phases for review:

- **Phase 1 (Tasks 1–14):** Non-coherent map/unmap end-to-end. No data transfer; apps that write to mapped memory don't see writes reach the receiver yet — harmless, not corrupting.
- **Phase 2 (Tasks 15–22):** Non-coherent flush/invalidate. Non-coherent fully spec-compliant; GPU readback works.
- **Phase 3a (Tasks 23–29):** Coherent C2.1 map/unmap bracketed copies. One-shot upload and one-shot download patterns work; persistent coherent maps with mid-map GPU writes still see stale data (covered by future C2.3).

**Out of scope for this plan (left as future work):**
- Phase 3b (C2.2 skip-copy-on-map flag)
- Phase 3c (C2.3 sync-point copies)
- Phase 3d (C2.4 diff-based wire optimization)
- Phase 4 (N4 page-protection dirty tracking, C3 hazard tracker, C4 source-side dirty tracking)
- Phase 5 (broader end-to-end test coverage)

---

## File Structure Overview

### Files to create — wire format and VM primitives

```
src/vkfwd/ferry/core/memory_map/
    vm_primitives.hpp                        # platform-abstracted VM reserve/commit/release
    vm_primitives_linux.cpp                  # Linux + Android impl
    vm_primitives_macos.cpp                  # macOS impl
    vm_primitives_windows.cpp                # Windows impl
    wire_format.hpp                          # MemoryMapRequest, MemoryMapResponse, MemoryUnmapRequestHeader, MemoryTransferRange,
                                             # MemoryFlushRequest, MemoryInvalidateRequest, MemoryInvalidateResponse,
                                             # QueryPhysicalDeviceMemoryInfoRequest, QueryPhysicalDeviceMemoryInfoResponse
    manual_dispatch.hpp                      # vkfwd::receiver::dispatch_manual_command declaration
    manual_dispatch.cpp                      # switch table over vkfwd::manual::CommandId, routes to MemoryMapReceiver / etc
    test/
        vm_primitives_test.cpp               # platform smoke test for reserve/commit/release
        wire_format_test.cpp                 # pack/unpack roundtrip for each manual wire struct
        manual_dispatch_test.cpp             # synthetic manual chunks routed through dispatcher
```

### Files to create — receiver hooks for handle map upkeep

```
src/vkfwd/ferry/receiver/hook/
    vkAllocateMemoryReceiverHook.hpp         # records source->receiver VkDeviceMemory in ReplayContext::handle_map
    vkFreeMemoryReceiverHook.hpp             # forgets source->receiver VkDeviceMemory in ReplayContext::handle_map
```

### Files to modify

```
src/vkfwd/ferry/core/CMakeLists.txt          # new sources for vm_primitives*, wire_format, manual_dispatch
src/vkfwd/ferry/receiver/CMakeLists.txt      # new receiver hooks (entries flow through __has_include)
src/vkfwd/ferry/script/generator/vulkan_metadata.py
    FORWARDER_MEMORY_MAP_MANAGED_COMMANDS = {"vkMapMemory", "vkUnmapMemory", "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges"}
    TARGET_COMMANDS += vkFlushMappedMemoryRanges, vkInvalidateMappedMemoryRanges
src/vkfwd/ferry/receiver/receiver.cpp        # check command_id range; route manual ids to dispatch_manual_command
src/vkfwd/ferry/receiver/replay_context.hpp  # add source->receiver handle maps
src/vkfwd/ferry/core/memory_map/manager.cpp  # extend MemoryMapReceiver::custom_*_endpoint to look up by source handle
src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.{hpp,cpp}    # real N2 impl
src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.{hpp,cpp}     # real N2 impl
src/vkfwd/ferry/core/memory_map/forwarder/coherent_allocation.{hpp,cpp}        # real C2.1 impl
src/vkfwd/ferry/core/memory_map/receiver/coherent_allocation.{hpp,cpp}         # real C2.1 impl
src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp               # add QueryPhysicalDeviceMemoryInfo fallback retry
src/vkfwd/ferry/core/custom_command.hpp                                        # add MemoryFlush, MemoryInvalidate manual command ids
```

### Files regenerated by `vulkan_metadata.py` (do not hand-edit)

```
src/vkfwd/ferry/forwarder/generated/entry/vkMapMemory_entry.cpp          # now delegates to MemoryMapForwarder::custom_vkMapMemory_entry
src/vkfwd/ferry/forwarder/generated/entry/vkUnmapMemory_entry.cpp        # delegates to custom_vkUnmapMemory_entry
src/vkfwd/ferry/forwarder/generated/entry/vkFlushMappedMemoryRanges_entry.cpp     # new — delegates to flush_ranges
src/vkfwd/ferry/forwarder/generated/entry/vkInvalidateMappedMemoryRanges_entry.cpp # new — delegates to invalidate_ranges
src/vkfwd/ferry/core/generated/command/vkFlushMappedMemoryRanges.cpp     # new
src/vkfwd/ferry/core/generated/command/vkInvalidateMappedMemoryRanges.cpp # new
```

---

## Namespace Convention

- Manual wire-format structs live in `vkfwd::memory_map::wire::` (e.g. `wire::MemoryMapRequest`).
- VM primitives live in `vkfwd::memory_map::vm::` with functions `vm::reserve()`, `vm::commit()`, `vm::release()`.
- Manual command dispatcher lives in `vkfwd::receiver::dispatch_manual_command(...)` (matches the existing `receiver::generated::call_api_endpoint` shape).

---

# PHASE 1 — Non-coherent N2 map/unmap end-to-end

## Task 1 — VM reserve/commit/release primitive

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/vm_primitives.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/vm_primitives_linux.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/vm_primitives_macos.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/vm_primitives_windows.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/test/vm_primitives_test.cpp`
- Modify: `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

The forwarder needs a 64-bit-host VM reserve+commit+release primitive that hides platform `#ifdef`. The reservation reserves the full `allocation_size` virtual address range; commits page-align the mapped sub-range. Returns page-aligned bases (always at least 4 KiB; 16 KiB on Apple Silicon; 64 KiB on Windows), satisfying `*ppData - offset` alignment by construction.

- [ ] **Step 1: Write the test**

Create `src/vkfwd/ferry/core/memory_map/test/vm_primitives_test.cpp`:

```cpp
#include "memory_map/vm_primitives.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

namespace vkfwd::memory_map::vm::test {

TEST_CASE("vm::reserve returns a page-aligned base or null") {
    constexpr std::size_t kReservationSize = 4 * 1024 * 1024; // 4 MiB
    void * const          base             = reserve(kReservationSize);
    REQUIRE(base != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(base) % 4096 == 0);
    release(base, kReservationSize);
}

TEST_CASE("vm::commit makes a sub-range readable and writable") {
    constexpr std::size_t kReservationSize = 1 * 1024 * 1024;
    void * const          base             = reserve(kReservationSize);
    REQUIRE(base != nullptr);

    constexpr std::size_t kCommitOffset = 4096; // page-aligned
    constexpr std::size_t kCommitSize   = 8192;

    REQUIRE(commit(static_cast<std::uint8_t *>(base) + kCommitOffset, kCommitSize));

    // Bytes must be readable and writable.
    auto * bytes = static_cast<std::uint8_t *>(base) + kCommitOffset;
    for (std::size_t i = 0; i < kCommitSize; ++i) { bytes[i] = static_cast<std::uint8_t>(i & 0xff); }
    for (std::size_t i = 0; i < kCommitSize; ++i) { CHECK(bytes[i] == static_cast<std::uint8_t>(i & 0xff)); }

    release(base, kReservationSize);
}

TEST_CASE("vm::release fully reclaims a reservation") {
    // Smoke test: reserve, commit, release, then reserve again at the same size.
    // The OS may give us the same VA back or a different one; either is fine.
    constexpr std::size_t kSize = 2 * 1024 * 1024;
    void *                first = reserve(kSize);
    REQUIRE(first != nullptr);
    release(first, kSize);

    void * second = reserve(kSize);
    REQUIRE(second != nullptr);
    release(second, kSize);
}

} // namespace vkfwd::memory_map::vm::test
```

- [ ] **Step 2: Add the test to the manifest**

Edit `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`:

```cmake
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  memory_type_registry_test.cpp
  vm_primitives_test.cpp)
```

- [ ] **Step 3: Verify the test fails to build**

```bash
dev/bin/build.py d
```

Expected: build error — `memory_map/vm_primitives.hpp` not found.

- [ ] **Step 4: Add the header**

Create `src/vkfwd/ferry/core/memory_map/vm_primitives.hpp`:

```cpp
#pragma once

#include <cstddef>

namespace vkfwd::memory_map::vm {

// Reserve `size` bytes of virtual address space. Returns a page-aligned base
// pointer, or nullptr on failure. The reservation charges no physical memory
// against the commit limit until commit() is called on a sub-range.
//
// On Linux/Android: mmap(NULL, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0)
// On macOS:        mmap(NULL, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
// On Windows:      VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS)
void * reserve(std::size_t size);

// Make the [base, base+size) range readable and writable. `base` and `size`
// must already be page-aligned by the caller — see page_floor/page_ceil
// helpers below. Returns true on success, false on commit failure (treat as
// VK_ERROR_OUT_OF_HOST_MEMORY at the call site).
//
// On Linux/Android/macOS: mprotect(base, size, PROT_READ|PROT_WRITE)
// On Windows:             VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE)
bool commit(void * base, std::size_t size);

// Release the entire reservation. `base` must be the value previously returned
// by reserve(); `size` must match the value previously passed to reserve().
//
// On Linux/Android/macOS: munmap(base, size)
// On Windows:             VirtualFree(base, 0, MEM_RELEASE)
void release(void * base, std::size_t size);

// Host page size (4096 on most platforms; 16384 on Apple Silicon and modern
// ARM64 Android; the OS's allocation granularity on Windows). Read once and
// cached.
std::size_t host_page_size();

inline std::size_t page_floor(std::size_t offset) {
    const std::size_t page = host_page_size();
    return offset & ~(page - 1);
}

inline std::size_t page_ceil(std::size_t offset) {
    const std::size_t page = host_page_size();
    return (offset + page - 1) & ~(page - 1);
}

} // namespace vkfwd::memory_map::vm
```

- [ ] **Step 5: Add the Linux + Android impl**

Create `src/vkfwd/ferry/core/memory_map/vm_primitives_linux.cpp`:

```cpp
#if defined(__linux__)
    #include "memory_map/vm_primitives.hpp"

    #include <sys/mman.h>
    #include <unistd.h>

namespace vkfwd::memory_map::vm {

std::size_t host_page_size() {
    // sysconf is reentrant and cheap; cache after first call.
    static const std::size_t cached = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return cached;
}

void * reserve(std::size_t size) {
    // MAP_NORESERVE: a PROT_NONE anonymous reservation should never charge the
    // commit limit, including under vm.overcommit_memory=2 (strict accounting).
    void * base = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { return nullptr; }
    return base;
}

bool commit(void * base, std::size_t size) {
    // mprotect can return ENOMEM under memory pressure (especially on
    // overcommit-disabled systems); treat any failure as out-of-host-memory at
    // the call site.
    return ::mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void release(void * base, std::size_t size) {
    ::munmap(base, size);
}

} // namespace vkfwd::memory_map::vm

#endif
```

- [ ] **Step 6: Add the macOS impl**

Create `src/vkfwd/ferry/core/memory_map/vm_primitives_macos.cpp`:

```cpp
#if defined(__APPLE__)
    #include "memory_map/vm_primitives.hpp"

    #include <sys/mman.h>
    #include <unistd.h>

namespace vkfwd::memory_map::vm {

std::size_t host_page_size() {
    // 16384 on Apple Silicon, 4096 on Intel macs. sysconf is reentrant.
    static const std::size_t cached = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return cached;
}

void * reserve(std::size_t size) {
    // No MAP_NORESERVE on macOS (or it's a no-op). PROT_NONE anonymous mappings
    // do not commit physical memory anyway.
    void * base = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { return nullptr; }
    return base;
}

bool commit(void * base, std::size_t size) {
    return ::mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void release(void * base, std::size_t size) {
    ::munmap(base, size);
}

} // namespace vkfwd::memory_map::vm

#endif
```

- [ ] **Step 7: Add the Windows impl**

Create `src/vkfwd/ferry/core/memory_map/vm_primitives_windows.cpp`:

```cpp
#if defined(_WIN32)
    #include "memory_map/vm_primitives.hpp"

    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>

namespace vkfwd::memory_map::vm {

std::size_t host_page_size() {
    // Use the *allocation granularity* (typically 64 KiB), not the page size
    // (typically 4 KiB). VirtualAlloc rounds MEM_RESERVE bases to allocation
    // granularity, so the page-floor/ceil math the rest of the code uses must
    // agree.
    static const std::size_t cached = []() {
        SYSTEM_INFO info {};
        ::GetSystemInfo(&info);
        return static_cast<std::size_t>(info.dwAllocationGranularity);
    }();
    return cached;
}

void * reserve(std::size_t size) {
    return ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool commit(void * base, std::size_t size) {
    return ::VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void release(void * base, std::size_t /*size*/) {
    // MEM_RELEASE requires size=0 — Windows release frees the whole reservation.
    ::VirtualFree(base, 0, MEM_RELEASE);
}

} // namespace vkfwd::memory_map::vm

#endif
```

- [ ] **Step 8: Wire the new sources into CMake**

In `src/vkfwd/ferry/core/CMakeLists.txt`, after `memory_map/receiver_allocation_factory.cpp`, add:

```cmake
memory_map/vm_primitives_linux.cpp
memory_map/vm_primitives_macos.cpp
memory_map/vm_primitives_windows.cpp
```

Each file's content is `#if`-gated, so all three compile into empty objects on the non-matching platforms — no platform `#ifdef` in CMake itself, no platform-specific source-list conditionals.

- [ ] **Step 9: Run the test suite**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all tests pass, including the three new vm_primitives tests.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
Add VM reserve/commit/release primitive for source staging

vm::reserve/commit/release wraps mmap+mprotect on POSIX and
VirtualAlloc/VirtualFree on Windows. PROT_NONE + MAP_NORESERVE
reservations cost only VA space, not physical memory, so a 4 GiB
allocation reserved by the Vulkan app costs ~4 GiB of source VA but no
RSS until the mapped sub-range is committed.

Page-aligned bases give us the *ppData - offset alignment contract for
free.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2 — Wire-format structs for MemoryMap / MemoryUnmap

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/wire_format.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/test/wire_format_test.cpp`
- Modify: `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`

The forwarder packs a `CommandChunkHeader` followed by a `MemoryMapRequest` (or `MemoryUnmapRequestHeader`); the receiver reads the same layout out of the request stream. These structs must be POD with explicit field ordering and no padding sensitivity — verified by static_assert and roundtrip test.

- [ ] **Step 1: Write the failing test**

Create `src/vkfwd/ferry/core/memory_map/test/wire_format_test.cpp`:

```cpp
#include "memory_map/wire_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace vkfwd::memory_map::wire::test {

TEST_CASE("MemoryMapRequest is POD and round-trips via memcpy") {
    MemoryMapRequest req {
        .manager_revision = kMemoryMapManagerRevision,
        .device           = reinterpret_cast<VkDevice>(0x1234),
        .memory           = reinterpret_cast<VkDeviceMemory>(0x5678),
        .offset           = 0x1000,
        .size             = VK_WHOLE_SIZE,
        .flags            = 0,
    };

    std::uint8_t buffer[sizeof(req)] {};
    std::memcpy(buffer, &req, sizeof(req));

    MemoryMapRequest out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == req.manager_revision);
    CHECK(out.device == req.device);
    CHECK(out.memory == req.memory);
    CHECK(out.offset == req.offset);
    CHECK(out.size == req.size);
    CHECK(out.flags == req.flags);
}

TEST_CASE("MemoryMapResponse round-trips via memcpy") {
    MemoryMapResponse resp {
        .manager_revision = kMemoryMapManagerRevision,
        .return_value     = VK_SUCCESS,
        .effective_size   = 0x4000,
    };

    std::uint8_t buffer[sizeof(resp)] {};
    std::memcpy(buffer, &resp, sizeof(resp));

    MemoryMapResponse out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == resp.manager_revision);
    CHECK(out.return_value == resp.return_value);
    CHECK(out.effective_size == resp.effective_size);
}

TEST_CASE("MemoryUnmapRequestHeader round-trips via memcpy") {
    MemoryUnmapRequestHeader header {
        .manager_revision = kMemoryMapManagerRevision,
        .device           = reinterpret_cast<VkDevice>(0x1234),
        .memory           = reinterpret_cast<VkDeviceMemory>(0x5678),
        .mapped_offset    = 0x2000,
        .mapped_size      = 0x4000,
        .range_count      = 0,
        .reserved         = 0,
    };

    std::uint8_t buffer[sizeof(header)] {};
    std::memcpy(buffer, &header, sizeof(header));

    MemoryUnmapRequestHeader out {};
    std::memcpy(&out, buffer, sizeof(out));

    CHECK(out.manager_revision == header.manager_revision);
    CHECK(out.mapped_offset == header.mapped_offset);
    CHECK(out.range_count == header.range_count);
}

} // namespace vkfwd::memory_map::wire::test
```

- [ ] **Step 2: Add to test manifest**

Edit `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`:

```cmake
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  memory_type_registry_test.cpp
  vm_primitives_test.cpp
  wire_format_test.cpp)
```

- [ ] **Step 3: Verify the test fails to build**

```bash
dev/bin/build.py d
```

Expected: build error — `memory_map/wire_format.hpp` not found.

- [ ] **Step 4: Add wire_format.hpp**

Create `src/vkfwd/ferry/core/memory_map/wire_format.hpp`:

```cpp
#pragma once

#include "memory_map/manager.hpp"  // kMemoryMapManagerRevision

#include <vulkan/vulkan.h>

#include <cstdint>
#include <type_traits>

namespace vkfwd::memory_map::wire {

// All wire structs are trivially-copyable POD so the receiver can read them by
// memcpy from the request stream. Every struct embeds manager_revision so a
// mismatched session is detected on the very first chunk.

struct MemoryMapRequest {
    std::uint32_t    manager_revision = kMemoryMapManagerRevision;
    std::uint32_t    pad0             = 0;  // alignment placeholder so 64-bit fields below align.
    VkDevice         device           = VK_NULL_HANDLE;
    VkDeviceMemory   memory           = VK_NULL_HANDLE;
    VkDeviceSize     offset           = 0;
    VkDeviceSize     size             = 0;
    VkMemoryMapFlags flags            = 0;
    std::uint32_t    pad1             = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapRequest>);

struct MemoryMapResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
    VkDeviceSize  effective_size   = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapResponse>);

struct MemoryUnmapRequestHeader {
    std::uint32_t  manager_revision = kMemoryMapManagerRevision;
    std::uint32_t  range_count      = 0;
    VkDevice       device           = VK_NULL_HANDLE;
    VkDeviceMemory memory           = VK_NULL_HANDLE;
    VkDeviceSize   mapped_offset    = 0;
    VkDeviceSize   mapped_size      = 0;
    std::uint32_t  reserved         = 0;
    std::uint32_t  pad0             = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryUnmapRequestHeader>);

// MemoryUnmapRequestHeader is followed by `range_count` of these, then the
// raw byte payloads each MemoryTransferRange.payload_offset points to. Phase 1
// always emits range_count == 0; Phase 3a's coherent unmap emits range_count == 1
// for the whole mapped range.
struct MemoryTransferRange {
    VkDeviceSize  offset         = 0;  // allocation-relative
    VkDeviceSize  size           = 0;
    std::uint64_t payload_offset = 0;  // relative to start of command chunk
};
static_assert(std::is_trivially_copyable_v<MemoryTransferRange>);

} // namespace vkfwd::memory_map::wire
```

- [ ] **Step 5: Run the test suite**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
Add wire-format structs for the manual memory-map protocol

MemoryMapRequest / MemoryMapResponse / MemoryUnmapRequestHeader /
MemoryTransferRange are POD with explicit padding so the receiver can
unpack via memcpy and the layout is stable across compilers.
manager_revision is embedded in every request/response so a session
on the wrong protocol revision fails on the first chunk.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3 — Source→receiver `VkDevice` and `VkDeviceMemory` handle translation

**Files:**
- Modify: `src/vkfwd/ferry/receiver/replay_context.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkAllocateMemoryReceiverHook.hpp`
- Create: `src/vkfwd/ferry/receiver/hook/vkFreeMemoryReceiverHook.hpp`
- Modify: `src/vkfwd/ferry/receiver/hook/vkCreateDeviceReceiverHook.hpp` (add device handle map upkeep alongside dispatch init)

The receiver needs to translate source-visible handles (which arrive in wire payloads) into the receiver-native handles real Vulkan accepts. We add two `std::unordered_map`s to `ReplayContext`:

```cpp
std::unordered_map<VkDevice, VkDevice>            source_to_receiver_device;
std::unordered_map<VkDeviceMemory, VkDeviceMemory> source_to_receiver_memory;
```

Populated at receiver-side `vkCreateDevice`/`vkAllocateMemory` (via `after_call` hooks reading `parameters.pDevice` source value vs `response.pDevice` receiver value), depopulated at `vkDestroyDevice`/`vkFreeMemory`. Real-Vulkan call sites translate via the map.

- [ ] **Step 1: Update `replay_context.hpp`**

Replace the existing struct body with:

```cpp
#pragma once

#include "generated/dispatch_table.hpp"
#include "memory_map/manager.hpp"

#include <vulkan/vulkan.h>

#include <unordered_map>

namespace vkfwd::receiver {

struct ReplayContext {
    // Receiver replay needs a mutable context instead of a bare const dispatch
    // table because successful create/destroy commands change what can be
    // replayed next.
    ::vkfwd::generated::DistributionTable dispatch {};

    // Per-context memory map manager: owns receiver-side mapped ranges and the
    // staging-byte transfer protocol for vkMapMemory / vkUnmapMemory.
    ::vkfwd::MemoryMapReceiver memoryMap;

    // Source-visible -> receiver-native handle translation. Source handles
    // arrive in wire payloads (including the manual memory-map command chunks);
    // the receiver MUST translate before calling any real Vulkan function.
    // Populated by after-call hooks on vkCreateDevice / vkAllocateMemory and
    // cleared by hooks on vkDestroyDevice / vkFreeMemory.
    std::unordered_map<VkDevice, VkDevice>             source_to_receiver_device;
    std::unordered_map<VkDeviceMemory, VkDeviceMemory> source_to_receiver_memory;
};

} // namespace vkfwd::receiver
```

- [ ] **Step 2: Write the receiver-side allocate hook test**

There's no existing receiver-hook unit-test scaffold; the cleanest approach is to add an in-process test under `src/vkfwd/ferry/receiver/test/` that synthesizes a `vkAllocateMemory` chunk and dispatches it through `call_api_endpoint`, then asserts on `ReplayContext::source_to_receiver_memory`.

Create `src/vkfwd/ferry/receiver/test/internal-test.cmake`:

```cmake
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  handle_map_test.cpp)
```

Create `src/vkfwd/ferry/receiver/test/handle_map_test.cpp`:

```cpp
#include "generated/command/vkAllocateMemory.hpp"
#include "generated/endpoints.hpp"
#include "replay_context.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::receiver::test {

namespace {
template<class Handle>
Handle test_handle(std::uintptr_t v) {
    return reinterpret_cast<Handle>(v);
}
} // namespace

TEST_CASE("vkAllocateMemoryReceiverHook records source->receiver memory mapping") {
    using Command = ::vkfwd::generated::commands::vkAllocateMemory::Command;

    ReplayContext context;
    // Synthesize a successful vkAllocateMemory chunk: parameters with the
    // source-visible VkDevice and a pAllocateInfo, then a response with the
    // receiver-native pMemory pointing at the receiver-issued handle.
    const VkDevice       source_device   = test_handle<VkDevice>(0x1001);
    const VkDeviceMemory source_memory   = test_handle<VkDeviceMemory>(0x2001);
    const VkDeviceMemory receiver_memory = test_handle<VkDeviceMemory>(0xDEAD2001);
    (void)source_memory;  // The source handle is set by the receiver after_call hook from response.pMemory.

    // For Phase 1, vkAllocateMemory's wire format uses the receiver-issued
    // handle as the source-visible handle (the source layer simply copies
    // *response.pMemory back into the app's *pMemory). The "source" and
    // "receiver" handles are therefore equal in this test — the entry exists
    // so future receivers that issue distinct handles (e.g. remote receivers
    // that share a process) Just Work without code changes.
    context.source_to_receiver_memory[receiver_memory] = receiver_memory;
    REQUIRE(context.source_to_receiver_memory.count(receiver_memory) == 1);
    context.source_to_receiver_memory.erase(receiver_memory);
    REQUIRE(context.source_to_receiver_memory.empty());
}

} // namespace vkfwd::receiver::test
```

Note: this is a structural test that only confirms the handle map is reachable from `ReplayContext`. The end-to-end behavior (hook fires on real `vkAllocateMemory_endpoint`) is exercised by the integration test in Task 14.

- [ ] **Step 3: Wire the receiver test manifest into CMake**

Update `src/vkfwd/ferry/receiver/CMakeLists.txt` so the `test/` directory's manifest is discovered. (The repo already auto-discovers `internal-test.cmake` manifests; verify after build that `handle_map_test.cpp` appears in the compiled test binary.)

- [ ] **Step 4: Add the receiver allocate hook**

Create `src/vkfwd/ferry/receiver/hook/vkAllocateMemoryReceiverHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkAllocateMemory.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::AllocateMemory> {
    static constexpr bool after_call_enabled = true;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }

    // Records the source-visible -> receiver-native VkDeviceMemory mapping so
    // future manual MemoryMap / MemoryUnmap chunks can translate the handle
    // before calling real Vulkan.
    static void after_call(const ::vkfwd::generated::commands::vkAllocateMemory::Command::Parameters & parameters,
                           const ::vkfwd::generated::commands::vkAllocateMemory::Command::Response &   response,
                           ::vkfwd::receiver::ReplayContext &                                          replay_context) {
        if (response.return_value != VK_SUCCESS || !parameters.pMemory || *parameters.pMemory == VK_NULL_HANDLE) { return; }
        // In Phase 1 the receiver and source share the same VkDeviceMemory
        // value because the receiver writes its handle back into pMemory which
        // the forwarder propagates to the app. The map still records the pair
        // so future receivers that mint distinct handles Just Work.
        replay_context.source_to_receiver_memory[*parameters.pMemory] = *parameters.pMemory;
    }
};

} // namespace vkfwd::receiver::manual
```

- [ ] **Step 5: Add the receiver free hook**

Create `src/vkfwd/ferry/receiver/hook/vkFreeMemoryReceiverHook.hpp`:

```cpp
#pragma once

#include "generated/command/vkFreeMemory.hpp"
#include "generated/receiver_hooks.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver::manual {

template<>
struct CommandHooks<::vkfwd::generated::CommandId::FreeMemory> {
    static constexpr bool before_call_enabled = true;

    template<class... Args>
    static constexpr void before_unpack(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_call(Args &...) noexcept {}
    template<class... Args>
    static constexpr void before_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr void after_pack_response(Args &...) noexcept {}
    template<class... Args>
    static constexpr bool replace_endpoint(Args &...) noexcept {
        return false;
    }

    static void before_call(const ::vkfwd::generated::commands::vkFreeMemory::Command::Parameters & parameters,
                            ::vkfwd::receiver::ReplayContext &                                       replay_context) {
        // before_call so the entry exists for the upcoming real-Vulkan
        // vkFreeMemory; we erase from our map first so a concurrent or stale
        // lookup cannot resolve a doomed handle.
        replay_context.source_to_receiver_memory.erase(parameters.memory);
    }
};

} // namespace vkfwd::receiver::manual
```

- [ ] **Step 6: Update vkCreateDeviceReceiverHook to track VkDevice in the map**

The existing hook's `after_call` only initializes the dispatch table. Add the handle-map insertion alongside:

```cpp
static void after_call(const ::vkfwd::generated::commands::vkCreateDevice::Command::Parameters & parameters,
                       const ::vkfwd::generated::commands::vkCreateDevice::Command::Response &   response,
                       ::vkfwd::receiver::ReplayContext &                                        replay_context) {
    if (response.return_value != VK_SUCCESS || !parameters.pDevice || *parameters.pDevice == VK_NULL_HANDLE) { return; }
    replay_context.dispatch.device.init(*parameters.pDevice, replay_context.dispatch.instance.get_device_proc_addr);
    // Same forwarder/receiver handle equivalence as vkAllocateMemoryReceiverHook.
    replay_context.source_to_receiver_device[*parameters.pDevice] = *parameters.pDevice;
}
```

- [ ] **Step 7: Run the test suite**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all tests pass.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
Add source->receiver handle map and receiver allocate/free hooks

ReplayContext gains source_to_receiver_device and
source_to_receiver_memory unordered_maps. Receiver hooks on
vkAllocateMemory / vkFreeMemory / vkCreateDevice keep them in sync.
Phase 1 manual MemoryMap / MemoryUnmap chunks translate through these
maps before calling real Vulkan.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4 — Manual command dispatch in the receiver

**Files:**
- Create: `src/vkfwd/ferry/core/memory_map/manual_dispatch.hpp`
- Create: `src/vkfwd/ferry/core/memory_map/manual_dispatch.cpp`
- Create: `src/vkfwd/ferry/core/memory_map/test/manual_dispatch_test.cpp`
- Modify: `src/vkfwd/ferry/core/memory_map/test/internal-test.cmake`
- Modify: `src/vkfwd/ferry/receiver/receiver.cpp`
- Modify: `src/vkfwd/ferry/core/CMakeLists.txt`

The receiver currently casts `header->command_id` to `generated::CommandId` unconditionally. We branch first on whether the id sits in the manual reserved range; if so, route through a new `dispatch_manual_command` function. Otherwise pass through to the generated dispatcher as today.

- [ ] **Step 1: Write the failing test**

Create `src/vkfwd/ferry/core/memory_map/test/manual_dispatch_test.cpp`:

```cpp
#include "command_id_range.hpp"
#include "command_stream.hpp"
#include "custom_command.hpp"
#include "memory_map/manual_dispatch.hpp"

#include <catch2/catch_test_macros.hpp>

namespace vkfwd::memory_map::test {

namespace {
::vkfwd::receiver::ReplayContext make_minimal_context() { return {}; }
} // namespace

TEST_CASE("dispatch_manual_command returns false for unknown manual ids") {
    auto context = make_minimal_context();
    CommandStream request;
    CommandStream response;
    const Range   range {.offset = 0, .size = 0};
    CHECK_FALSE(::vkfwd::receiver::dispatch_manual_command(static_cast<::vkfwd::manual::CommandId>(::vkfwd::kReservedCommandIdBase + 0xff),
                                                            request, range, response, context));
}

} // namespace vkfwd::memory_map::test
```

- [ ] **Step 2: Update test manifest**

```cmake
set(VKFWD_INTERNAL_TEST_LOCAL_SOURCES
  memory_type_registry_test.cpp
  vm_primitives_test.cpp
  wire_format_test.cpp
  manual_dispatch_test.cpp)
```

- [ ] **Step 3: Verify the test fails to build**

```bash
dev/bin/build.py d
```

Expected: build error — `memory_map/manual_dispatch.hpp` not found.

- [ ] **Step 4: Add the header**

Create `src/vkfwd/ferry/core/memory_map/manual_dispatch.hpp`:

```cpp
#pragma once

#include "command_stream.hpp"
#include "custom_command.hpp"

namespace vkfwd::receiver {
struct ReplayContext;
}

namespace vkfwd::receiver {

// Routes vkfwd-owned manual command ids (the [kReservedCommandIdBase, 2^32)
// range) to the matching handler. Returns true if the manual id was recognized
// and the handler succeeded; false otherwise (the receiver session aborts the
// stream on false to surface protocol errors loudly).
bool dispatch_manual_command(::vkfwd::manual::CommandId command_id, const CommandStream & request_stream, const Range & request_range,
                              CommandStream & response_stream, ReplayContext & replay_context);

} // namespace vkfwd::receiver
```

- [ ] **Step 5: Add the implementation**

Create `src/vkfwd/ferry/core/memory_map/manual_dispatch.cpp`:

```cpp
#include "memory_map/manual_dispatch.hpp"

#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "replay_context.hpp"

namespace vkfwd::receiver {

bool dispatch_manual_command(::vkfwd::manual::CommandId command_id, const CommandStream & request_stream, const Range & request_range,
                              CommandStream & response_stream, ReplayContext & replay_context) {
    switch (command_id) {
        case ::vkfwd::manual::CommandId::MemoryMap:
            return replay_context.memoryMap.custom_vkMapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
        case ::vkfwd::manual::CommandId::MemoryUnmap:
            return replay_context.memoryMap.custom_vkUnmapMemory_endpoint(request_stream, request_range, response_stream, replay_context);
        case ::vkfwd::manual::CommandId::QueryPhysicalDeviceMemoryInfo:
            // Implementation lands in Task 12.
            VKFWD_LOG_ERROR("vkfwd receiver: manual::CommandId::QueryPhysicalDeviceMemoryInfo not yet wired");
            return false;
    }
    VKFWD_LOG_ERROR("vkfwd receiver: unknown manual command_id={}", static_cast<std::uint32_t>(command_id));
    return false;
}

} // namespace vkfwd::receiver
```

NOTE: `MemoryMapReceiver::custom_vkMapMemory_endpoint` and `custom_vkUnmapMemory_endpoint` will gain a `ReplayContext &` parameter in Task 9; for now the call sites above already pass it through, and Task 5 updates the manager header + body to accept it. The intermediate build state between Tasks 4 and 5 will not compile; commit Task 4 and Task 5 together if you cannot tolerate a transient broken intermediate.

- [ ] **Step 6: Update `MemoryMapReceiver::custom_vkMapMemory_endpoint` / `custom_vkUnmapMemory_endpoint` signatures to take `ReplayContext &`**

In `src/vkfwd/ferry/core/memory_map/manager.hpp`, add forward decl `namespace vkfwd::receiver { struct ReplayContext; }` at the top, and change the two custom endpoint signatures:

```cpp
bool custom_vkMapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                  ::vkfwd::receiver::ReplayContext & replay_context);
bool custom_vkUnmapMemory_endpoint(const CommandStream & request_stream, const Range & request_range, CommandStream & response_stream,
                                    ::vkfwd::receiver::ReplayContext & replay_context);
```

In `manager.cpp`, mirror the signature change on `MemoryMapReceiver::Impl` and the outer facade. Phase-0 placeholder bodies still return `false` — they just gain an unused `ReplayContext &` parameter.

- [ ] **Step 7: Wire manual dispatch into receiver.cpp**

Edit `src/vkfwd/ferry/receiver/receiver.cpp`. After reading `header->command_id`, branch on the reserved range:

```cpp
#include "command_id_range.hpp"
#include "custom_command.hpp"
#include "memory_map/manual_dispatch.hpp"

// ... inside the per-chunk loop in receive_accumulated_api_calls() ...

const std::uint32_t raw_command_id = header->command_id;
const Range request_range {
    .offset = offset,
    .size   = header->size,
};

bool ok = false;
if (raw_command_id >= ::vkfwd::kReservedCommandIdBase) {
    ok = ::vkfwd::receiver::dispatch_manual_command(static_cast<::vkfwd::manual::CommandId>(raw_command_id),
                                                     request_stream, request_range, response_stream, replay_context_);
} else {
    const auto command_id = static_cast<generated::CommandId>(raw_command_id);
    ok                    = receiver::generated::call_api_endpoint(command_id, request_stream, request_range, response_stream, replay_context_);
}
if (!ok) {
    VKFWD_LOG_ERROR("vkfwd receiver: failed to dispatch command_id={}", raw_command_id);
    return response_stream;
}
```

- [ ] **Step 8: Add the new source to CMake**

In `src/vkfwd/ferry/core/CMakeLists.txt`, add `memory_map/manual_dispatch.cpp` to the source list.

- [ ] **Step 9: Run the test suite**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all tests pass, including the new manual_dispatch_test.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
Add manual command dispatch path in the receiver

receiver.cpp now branches by command_id range: ids in
[kReservedCommandIdBase, 2^32) go through vkfwd::receiver::
dispatch_manual_command, which routes to MemoryMapReceiver (for the two
memory-map ids) or returns false (and logs) for unknown manual ids. The
generated command-id dispatcher is unchanged.

MemoryMapReceiver::custom_*_endpoint methods now accept ReplayContext &
so receiver-side endpoints can reach the source->receiver handle map and
the dispatch table.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5 — Enable `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS` for vkMapMemory + vkUnmapMemory

**Files:**
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`
- Regenerate: `src/vkfwd/ferry/forwarder/generated/entry/vkMapMemory_entry.cpp`, `vkUnmapMemory_entry.cpp`

The generator's `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS` set controls which Vulkan entry points delegate to `MemoryMapForwarder::custom_*_entry`. Currently empty — the entries still emit the generated command path. Adding `vkMapMemory` and `vkUnmapMemory` to the set causes the generator to emit delegation code instead.

- [ ] **Step 1: Inspect the current generator template branch**

```bash
grep -n 'FORWARDER_MEMORY_MAP_MANAGED_COMMANDS\|custom_vkMapMemory_entry\|custom_vkUnmapMemory_entry' src/vkfwd/ferry/script/generator/vulkan_metadata.py
```

Find the template branch around line 1896 that the generator uses when a command is in `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS`. Confirm it emits a body that calls `::vkfwd::MemoryMapForwarder::instance().custom_vkMapMemory_entry(...)` / `custom_vkUnmapMemory_entry(...)`. If the template doesn't yet exist (Phase 0 left the set empty so the template path was never exercised), add it now — the template should produce a function body shaped like:

```cpp
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory_entry(VkDevice device, VkDeviceMemory memory,
                                                 VkDeviceSize offset, VkDeviceSize size,
                                                 VkMemoryMapFlags flags, void ** ppData) {
    return ::vkfwd::MemoryMapForwarder::instance().custom_vkMapMemory_entry(device, memory, offset, size, flags, ppData);
}
```

(and the void-returning variant for `vkUnmapMemory_entry`).

- [ ] **Step 2: Enable the two commands**

In `src/vkfwd/ferry/script/generator/vulkan_metadata.py`, find:

```python
FORWARDER_MEMORY_MAP_MANAGED_COMMANDS = set()
```

Change to:

```python
FORWARDER_MEMORY_MAP_MANAGED_COMMANDS = {"vkMapMemory", "vkUnmapMemory"}
```

- [ ] **Step 3: Regenerate**

```bash
python3 src/vkfwd/ferry/script/generator/vulkan_metadata.py
```

Verify `src/vkfwd/ferry/forwarder/generated/entry/vkMapMemory_entry.cpp` and `vkUnmapMemory_entry.cpp` now delegate to `MemoryMapForwarder::instance().custom_*_entry`. The existing `__has_include("hook/vkMapMemoryForwarderHook.hpp")` block can stay if it's harmless (the hook body becomes dead code for the delegated path, or you can update the hook headers in a follow-up to just be empty stubs).

- [ ] **Step 4: Verify the existing forwarder map/unmap tests still pass**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all tests pass. The `vkMapMemory_test` in particular: it doesn't allocate first, so `MemoryMapForwarder::custom_vkMapMemory_entry` returns `VK_ERROR_FEATURE_NOT_PRESENT` from the no-record path — same end-user behavior as before.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
Generator: route vkMapMemory/vkUnmapMemory through MemoryMapForwarder

FORWARDER_MEMORY_MAP_MANAGED_COMMANDS gains vkMapMemory and vkUnmapMemory.
The generated entry points now delegate directly to
MemoryMapForwarder::custom_*_entry instead of emitting the standard
generated Vulkan command path. The custom entries still return
VK_ERROR_FEATURE_NOT_PRESENT for unrecorded handles, so behavior is
unchanged until the per-strategy NonCoherent / Coherent implementations
ship.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6 — Carry classification info on the MemoryMap wire chunk

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/wire_format.hpp`
- Modify: `src/vkfwd/ferry/core/memory_map/test/wire_format_test.cpp`

The receiver does not yet have a `MemoryTypeRegistry`. Phase 1 puts classification (property flags, atom size, alignment, allocation size) into the MemoryMapRequest so the receiver can lazily create the matching `ReceiverAllocation` on first map. The forwarder already knows all of this from the registry resolve at allocate time; it carries the resolved values into the wire payload.

- [ ] **Step 1: Extend MemoryMapRequest**

Edit `src/vkfwd/ferry/core/memory_map/wire_format.hpp`:

```cpp
struct MemoryMapRequest {
    std::uint32_t         manager_revision  = kMemoryMapManagerRevision;
    std::uint32_t         memory_type_index = 0;
    VkDevice              device            = VK_NULL_HANDLE;
    VkDeviceMemory        memory            = VK_NULL_HANDLE;
    VkDeviceSize          offset            = 0;
    VkDeviceSize          size              = 0;  // VK_WHOLE_SIZE allowed
    VkMemoryMapFlags      flags             = 0;
    std::uint32_t         pad0              = 0;
    // Forwarder-resolved classification, carried so the receiver can lazily
    // construct the matching ReceiverAllocation strategy without its own
    // MemoryTypeRegistry. Stored on every map request (not just the first) so
    // the receiver does not need to track "have I created the allocation yet".
    VkMemoryPropertyFlags property_flags          = 0;
    std::uint32_t         pad1                    = 0;
    VkDeviceSize          allocation_size         = 0;
    VkDeviceSize          non_coherent_atom_size  = 0;
    std::uint64_t         min_memory_map_alignment = 0;
};
static_assert(std::is_trivially_copyable_v<MemoryMapRequest>);
```

- [ ] **Step 2: Extend the roundtrip test**

In `wire_format_test.cpp` `MemoryMapRequest` test, add checks for the new fields:

```cpp
MemoryMapRequest req {
    .manager_revision         = kMemoryMapManagerRevision,
    .memory_type_index        = 3,
    .device                   = reinterpret_cast<VkDevice>(0x1234),
    .memory                   = reinterpret_cast<VkDeviceMemory>(0x5678),
    .offset                   = 0x1000,
    .size                     = VK_WHOLE_SIZE,
    .flags                    = 0,
    .property_flags           = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    .allocation_size          = 0x100000,
    .non_coherent_atom_size   = 64,
    .min_memory_map_alignment = 4096,
};
// ... add CHECK lines for each new field ...
```

- [ ] **Step 3: Run tests and commit**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
git add -A
git commit -m "Extend MemoryMapRequest with classification fields"
```

---

## Task 7 — `NonCoherentForwarderAllocation::map()` real implementation

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.hpp`
- Modify: `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.cpp`
- Create: `src/vkfwd/ferry/forwarder/test/non_coherent_map_test.cpp`
- Modify: `src/vkfwd/ferry/forwarder/test/internal-test.cmake`

Implements the algorithm sketched in `doc/memory_map_management.md` § "NonCoherentForwarderAllocation::map(offset, size, flags, ppData)". Local-first ordering: reserve+commit before sending the wire chunk; rejected receiver-side maps release the reservation; effective_size mismatch fails the call with `VK_ERROR_UNKNOWN`.

- [ ] **Step 1: Update the header with per-allocation state**

```cpp
#pragma once

#include "memory_map/forwarder_allocation.hpp"

#include <cstddef>

namespace vkfwd::memory_map {

class NonCoherentForwarderAllocation final : public ForwarderAllocation {
public:
    using ForwarderAllocation::ForwarderAllocation;

    VkResult map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) override;
    void     unmap() override;
    VkResult flush(VkDeviceSize offset, VkDeviceSize size) override;
    VkResult invalidate(VkDeviceSize offset, VkDeviceSize size) override;

private:
    // Active mapping state. reservation_base_ != nullptr iff the allocation
    // is currently mapped. Cleared on unmap().
    void *       reservation_base_ = nullptr;
    VkDeviceSize mapped_offset_    = 0;
    VkDeviceSize mapped_size_      = 0;
};

} // namespace vkfwd::memory_map
```

- [ ] **Step 2: Write the test cases**

Create `src/vkfwd/ferry/forwarder/test/non_coherent_map_test.cpp`. Each `TEST_CASE` exercises one branch of the algorithm sketch:

```cpp
// Test cases to author:
//
// 1. map success: reserve+commit happens BEFORE the wire chunk; the response
//    handler verifies the request stream contains a manual MemoryMap chunk
//    with the right classification fields; *ppData == reservation_base + offset.
// 2. map alignment: across a sweep of offsets including unaligned values,
//    (*ppData - offset) is a multiple of min_memory_map_alignment.
// 3. local failure ordering: install a vm::reserve / vm::commit failure
//    injection (introduced via a test-only hook in vm_primitives.hpp guarded
//    by an inline std::function pointer that defaults to nullptr); assert
//    no MemoryMap chunk reached the wire and the call returns
//    VK_ERROR_OUT_OF_HOST_MEMORY.
// 4. receiver rejection cleanup: response carries VK_ERROR_OUT_OF_DEVICE_MEMORY;
//    assert the source releases the reservation and propagates the receiver's
//    error.
// 5. effective_size mismatch: response carries a different effective_size than
//    the source resolved; assert VK_ERROR_UNKNOWN and reservation released.
// 6. revision mismatch: response carries manager_revision != current; assert
//    VK_ERROR_UNKNOWN (the per-call protocol-error variant — full session-fatal
//    handling is Phase 2/3 work).
```

Each test follows the existing `install_pack_unpack_transport` pattern. Synthesizing responses requires building a `CommandStream` whose payload starts with `wire::MemoryMapResponse` immediately after a `CommandChunkHeader` for `manual::CommandId::MemoryMap` — write a small `pack_manual_response` helper in this test file. (Full code for one of these tests is shown below; the remaining 5 follow the same shape.)

```cpp
// Full code for test case 1.
TEST_CASE("NonCoherentForwarderAllocation::map reserves staging then sends MemoryMap then writes *ppData") {
    // Synthesize a primed registry + allocation via the existing
    // vkAllocateFreeMemory_test pattern, then drive vkMapMemory_entry on the
    // recorded handle. Assert the request stream contains a manual chunk with
    // manual::CommandId::MemoryMap and the right classification fields.
    // ... (full body to be written by the implementer; ~80 lines including the
    // packing helper) ...
}
```

- [ ] **Step 3: Implement `map()`**

In `non_coherent_allocation.cpp`:

```cpp
#include "memory_map/forwarder/non_coherent_allocation.hpp"

#include "command_stream.hpp"
#include "custom_command.hpp"
#include "forwarder.hpp"
#include "logging.hpp"
#include "memory_map/manager.hpp"
#include "memory_map/vm_primitives.hpp"
#include "memory_map/wire_format.hpp"

#include <cstring>

namespace vkfwd::memory_map {

namespace {
constexpr std::size_t kCommandChunkHeaderSize = sizeof(::vkfwd::CommandChunkHeader);
}

VkResult NonCoherentForwarderAllocation::map(VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void ** ppData) {
    // Step 1: resolve effective_size against the recorded allocation extent.
    const VkDeviceSize allocation_size = info().allocation_size;
    if (offset > allocation_size) { return VK_ERROR_MEMORY_MAP_FAILED; }
    const VkDeviceSize effective_size = (size == VK_WHOLE_SIZE) ? (allocation_size - offset) : size;
    if (offset + effective_size > allocation_size) { return VK_ERROR_MEMORY_MAP_FAILED; }

    // Step 2: reserve full allocation_size of VA space.
    void * const reservation = vm::reserve(static_cast<std::size_t>(allocation_size));
    if (!reservation) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map vm::reserve failed for allocation_size={}", allocation_size);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Step 3: commit the page-aligned span covering the mapped sub-range.
    const std::size_t commit_begin = vm::page_floor(static_cast<std::size_t>(offset));
    const std::size_t commit_end   = vm::page_ceil(static_cast<std::size_t>(offset + effective_size));
    if (!vm::commit(static_cast<std::uint8_t *>(reservation) + commit_begin, commit_end - commit_begin)) {
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    // Step 4: build the manual MemoryMap request chunk and append to the
    // request stream.
    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    ::vkfwd::CommandChunkHeader chunk_header {};
    chunk_header.command_id = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryMap);
    chunk_header.size       = kCommandChunkHeaderSize + sizeof(wire::MemoryMapRequest);
    auto header_view        = stream.grow(kCommandChunkHeaderSize);
    std::memcpy(header_view.address(0), &chunk_header, kCommandChunkHeaderSize);

    wire::MemoryMapRequest req {
        .manager_revision         = kMemoryMapManagerRevision,
        .memory_type_index        = info().memory_type_index,
        .device                   = info().device,
        .memory                   = info().memory,
        .offset                   = offset,
        .size                     = effective_size,  // resolved by us; receiver still re-resolves and validation in step 7 catches divergence.
        .flags                    = flags,
        .property_flags           = info().property_flags,
        .allocation_size          = allocation_size,
        .non_coherent_atom_size   = info().non_coherent_atom_size,
        .min_memory_map_alignment = info().min_memory_map_alignment,
    };
    auto req_view = stream.grow(sizeof(req));
    std::memcpy(req_view.address(0), &req, sizeof(req));

    // Step 5: flush — synchronous round-trip.
    CommandStream response_stream = forwarder.flush();
    if (response_stream.size() < sizeof(wire::MemoryMapResponse)) {
        VKFWD_LOG_ERROR("vkfwd: NonCoherentForwarderAllocation::map response too small ({} bytes)", response_stream.size());
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    wire::MemoryMapResponse response {};
    std::memcpy(&response, response_stream.at(0, sizeof(response)).address(0), sizeof(response));

    // Step 7: validate.
    if (response.manager_revision != kMemoryMapManagerRevision) {
        VKFWD_LOG_ERROR("vkfwd: MemoryMap response manager_revision mismatch ({} vs {})", response.manager_revision, kMemoryMapManagerRevision);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }
    if (response.return_value != VK_SUCCESS) {
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return static_cast<VkResult>(response.return_value);
    }
    if (response.effective_size != effective_size) {
        VKFWD_LOG_ERROR("vkfwd: MemoryMap effective_size mismatch ({} vs {})", response.effective_size, effective_size);
        vm::release(reservation, static_cast<std::size_t>(allocation_size));
        return VK_ERROR_UNKNOWN;
    }

    // Step 9: success — record state.
    reservation_base_ = reservation;
    mapped_offset_    = offset;
    mapped_size_      = effective_size;

    if (ppData) { *ppData = static_cast<std::uint8_t *>(reservation_base_) + offset; }
    return VK_SUCCESS;
}

void NonCoherentForwarderAllocation::unmap() { /* implemented in Task 8 */ }
VkResult NonCoherentForwarderAllocation::flush(VkDeviceSize, VkDeviceSize) { return VK_ERROR_FEATURE_NOT_PRESENT; /* Phase 2 */ }
VkResult NonCoherentForwarderAllocation::invalidate(VkDeviceSize, VkDeviceSize) { return VK_ERROR_FEATURE_NOT_PRESENT; /* Phase 2 */ }

} // namespace vkfwd::memory_map
```

- [ ] **Step 4: Run tests**

```bash
dev/bin/build.py d
python3 dev/bin/cit.py --build-dir build/macos.clang.debug
```

Expected: all map test cases pass.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "Implement NonCoherentForwarderAllocation::map (N2 algorithm)"
```

---

## Task 8 — `NonCoherentForwarderAllocation::unmap()` real implementation

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/forwarder/non_coherent_allocation.cpp`
- Create: `src/vkfwd/ferry/forwarder/test/non_coherent_unmap_test.cpp`
- Modify: `src/vkfwd/ferry/forwarder/test/internal-test.cmake`

Sends a header-only MemoryUnmap chunk (range_count = 0 under N2). After the synchronous flush returns, releases the reservation. Re-mapping the same handle after unmap re-runs the full reserve+commit cycle.

- [ ] **Step 1: Write the test cases** (similar shape to Task 7's tests)
  - unmap sends manual::CommandId::MemoryUnmap with range_count = 0 and no payload
  - reservation is released only after flush() returns
  - re-map after unmap acquires fresh staging cleanly

- [ ] **Step 2: Implement `unmap()`**

```cpp
void NonCoherentForwarderAllocation::unmap() {
    if (!reservation_base_) { return; }  // never mapped or already unmapped

    auto & forwarder = ::vkfwd::Forwarder::instance();
    auto & stream    = forwarder.request_stream();

    ::vkfwd::CommandChunkHeader chunk_header {};
    chunk_header.command_id = static_cast<std::uint32_t>(::vkfwd::manual::CommandId::MemoryUnmap);
    chunk_header.size       = kCommandChunkHeaderSize + sizeof(wire::MemoryUnmapRequestHeader);
    auto header_view        = stream.grow(kCommandChunkHeaderSize);
    std::memcpy(header_view.address(0), &chunk_header, kCommandChunkHeaderSize);

    wire::MemoryUnmapRequestHeader unmap_header {
        .manager_revision = kMemoryMapManagerRevision,
        .range_count      = 0,
        .device           = info().device,
        .memory           = info().memory,
        .mapped_offset    = mapped_offset_,
        .mapped_size      = mapped_size_,
        .reserved         = 0,
        .pad0             = 0,
    };
    auto unmap_view = stream.grow(sizeof(unmap_header));
    std::memcpy(unmap_view.address(0), &unmap_header, sizeof(unmap_header));

    (void)forwarder.flush();  // synchronous; receiver has unmapped before return.

    vm::release(reservation_base_, static_cast<std::size_t>(info().allocation_size));
    reservation_base_ = nullptr;
    mapped_offset_    = 0;
    mapped_size_      = 0;
}
```

- [ ] **Step 3: Run tests + commit**

---

## Task 9 — `NonCoherentReceiverAllocation::map_endpoint()` real implementation

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.hpp` (add private state for receiver_ptr)
- Modify: `src/vkfwd/ferry/core/memory_map/receiver/non_coherent_allocation.cpp`
- Modify: `src/vkfwd/ferry/core/memory_map/manager.cpp` — `MemoryMapReceiver::Impl::custom_vkMapMemory_endpoint` now unpacks the chunk, looks up (or creates) the per-handle entry, and delegates.

The receiver-side endpoint unpacks `MemoryMapRequest`, looks up the receiver-native `VkDeviceMemory` via `ReplayContext::source_to_receiver_memory`, calls real `vkMapMemory(receiver_device, receiver_memory, ...)`, stores `receiver_ptr` privately on the `NonCoherentReceiverAllocation`, and packs a `MemoryMapResponse` carrying only the return value and effective_size — never the pointer.

- [ ] **Step 1: Lazy-create the receiver allocation on first map**

`MemoryMapReceiver::Impl::custom_vkMapMemory_endpoint(...)` unpacks `MemoryMapRequest`, builds a `ReceiverAllocation::CreationInfo` from the request's classification fields, calls `ReceiverAllocationFactory::create(...)`, and inserts into the per-handle map keyed by source `VkDeviceMemory`. If the handle is already in the map, reuse it. Then delegates to `allocation->map_endpoint(...)`.

- [ ] **Step 2: Implement `NonCoherentReceiverAllocation::map_endpoint()`**

```cpp
bool NonCoherentReceiverAllocation::map_endpoint(const CommandStream & request_stream, const Range & request_range,
                                                  CommandStream & response_stream, ::vkfwd::receiver::ReplayContext & replay_context) {
    // ... unpack MemoryMapRequest from request_range ...
    // ... look up receiver_device = replay_context.source_to_receiver_device.at(req.device) ...
    // ... look up receiver_memory = replay_context.source_to_receiver_memory.at(req.memory) ...
    // ... resolve effective_size (handle VK_WHOLE_SIZE) ...
    // ... call replay_context.dispatch.device.map_memory(receiver_device, receiver_memory, req.offset, effective_size, req.flags, &receiver_ptr_) ...
    // ... pack MemoryMapResponse {manager_revision, return_value, effective_size} into response_stream ...
    // ... return true (false only on protocol errors like bad chunk size, not on driver errors) ...
}
```

Full ~80-line implementation follows the sketch directly. `receiver_ptr_` is a new private member on `NonCoherentReceiverAllocation`.

- [ ] **Step 3+: Tests, commit (same shape as previous tasks)**

---

## Task 10 — `NonCoherentReceiverAllocation::unmap_endpoint()` real implementation

Mirror of Task 9 for unmap. Under N2 there are zero payload ranges to copy, so the body is: unpack `MemoryUnmapRequestHeader`, look up receiver_memory, call real `vkUnmapMemory(receiver_device, receiver_memory)`, clear `receiver_ptr_`, return true.

---

## Task 11 — Plumbing: MemoryMapReceiver tracks per-handle allocations through manual MemoryMap chunk

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/manager.cpp`

The receiver `MemoryMapReceiver::Impl::custom_vkMapMemory_endpoint` (Task 9 step 1) creates the `ReceiverAllocation` lazily from the wire payload's classification fields. The matching `forget_allocation` happens on the manual MemoryUnmap chunk (the unmap is the natural end-of-mapping point), or — for safety — on `vkFreeMemory`'s receiver-side hook. Add the latter as a defense-in-depth check.

---

## Task 12 — `QueryPhysicalDeviceMemoryInfo` fallback (forwarder side)

**Files:**
- Modify: `src/vkfwd/ferry/forwarder/hook/vkAllocateMemoryForwarderHook.hpp` — add the fallback retry around the registry resolve
- Modify: `src/vkfwd/ferry/core/memory_map/wire_format.hpp` — add QueryPhysicalDeviceMemoryInfoRequest/Response (already shown in design doc § "QueryPhysicalDeviceMemoryInfo")
- Test: synthesize a cache-miss path; the test transport answers the fallback chunk; assert the registry is now populated and the allocation is recorded

---

## Task 13 — `QueryPhysicalDeviceMemoryInfo` answer (receiver side)

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/manual_dispatch.cpp` — implement the QueryPhysicalDeviceMemoryInfo branch
- Receiver implementation: call real `vkGetPhysicalDeviceMemoryProperties(physical_device, ...)` + `vkGetPhysicalDeviceProperties(...)`, populate the response, write it to `response_stream`

The receiver also needs source→receiver `VkPhysicalDevice` translation — add a third unordered_map to `ReplayContext` populated by an `after_call` hook on `vkEnumeratePhysicalDevices`.

---

## Task 14 — Phase 1 end-to-end loopback test

**Files:**
- Create: `src/vkfwd/ferry/test/phase1_loopback_test.cpp` (similar to `create-instance-test.cpp`)

Uses `loopback_runtime.hpp` + rapid-vulkan to:
1. Create instance + device through the forwarder layer
2. Allocate non-coherent host-visible memory via the layer
3. Map + unmap via the layer
4. Assert no crashes, no leaks, return values are `VK_SUCCESS`
5. Assert that bytes written to mapped memory do NOT appear in receiver-side memory (Phase 1 N2: no transfer at map/unmap)

This locks in the "Phase 1 N2 is intentionally lossy" invariant so it doesn't regress when Phase 2 adds flush.

**🛑 Phase 1 stop point — review before continuing to Phase 2.**

---

# PHASE 2 — Non-coherent flush/invalidate

## Task 15 — Generator: add `vkFlushMappedMemoryRanges` + `vkInvalidateMappedMemoryRanges`

**Files:**
- Modify: `src/vkfwd/ferry/script/generator/vulkan_metadata.py`

Add both command names to `TARGET_COMMANDS` so the generator emits Command pack/unpack, entry points, and endpoint stubs. Add both to `FORWARDER_MEMORY_MAP_MANAGED_COMMANDS` so the entry points delegate to `MemoryMapForwarder::flush_ranges`/`invalidate_ranges` (already on the manager surface from Phase 0).

Regenerate. Verify the entry sources delegate cleanly.

---

## Task 16 — Wire format for flush/invalidate

**Files:**
- Modify: `src/vkfwd/ferry/core/custom_command.hpp` — add `MemoryFlush`, `MemoryInvalidate` manual ids
- Modify: `src/vkfwd/ferry/core/memory_map/wire_format.hpp`

```cpp
struct MemoryFlushRangeEntry {
    VkDeviceMemory memory         = VK_NULL_HANDLE;
    VkDeviceSize   offset         = 0;
    VkDeviceSize   size           = 0;
    std::uint64_t  payload_offset = 0;  // relative to start of command chunk
};

struct MemoryFlushRequestHeader {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::uint32_t range_count      = 0;
    VkDevice      device           = VK_NULL_HANDLE;
};

struct MemoryFlushResponse {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
};

// MemoryInvalidate mirrors the flush layout — header + range entries on the
// request side, but the response carries the receiver-read bytes as a payload
// following the response struct. The forwarder copies those bytes back into
// its staging.
struct MemoryInvalidateRequestHeader { /* same fields as MemoryFlushRequestHeader */ };
struct MemoryInvalidateRangeEntry {    /* same as MemoryFlushRangeEntry but payload_offset is in the response */ };
struct MemoryInvalidateResponseHeader {
    std::uint32_t manager_revision = kMemoryMapManagerRevision;
    std::int32_t  return_value     = VK_SUCCESS;
    std::uint32_t range_count      = 0;
};
```

(Full details + roundtrip tests in the same shape as Task 2.)

---

## Task 17 — Forwarder entry hooks for flush/invalidate

Generated entry points delegate to `MemoryMapForwarder::flush_ranges` / `invalidate_ranges`, which were added in Phase 0 and dispatch per-range to `ForwarderAllocation::flush` / `invalidate`. Tests verify per-range dispatch.

---

## Task 18 — `NonCoherentForwarderAllocation::flush(offset, size)` real implementation

Copy the bytes from source staging at `[offset, offset+size)`, build a `MemoryFlush` chunk with one range entry pointing at those bytes, send via `forwarder.flush()`, read back the response (revision + return code), return.

Tests: write source bytes → flush → assert receiver-side handler observes the expected bytes; flush of an unmapped range returns VK_ERROR_MEMORY_MAP_FAILED; nonCoherentAtomSize alignment.

---

## Task 19 — `NonCoherentForwarderAllocation::invalidate(offset, size)` real implementation

Build a `MemoryInvalidate` chunk with one range entry, send via `forwarder.flush()`, read back the response which carries the receiver-side bytes, copy them into source staging at `[offset, offset+size)`.

Tests: receiver-side writes → invalidate → assert source staging has the new bytes.

---

## Task 20 — `NonCoherentReceiverAllocation::flush_endpoint()` real implementation

Unpack the MemoryFlush header + range entries, copy each payload into the receiver mapped pointer at `(range.offset - mapped_offset)`, call real `vkFlushMappedMemoryRanges` for the copied ranges (atom-size-aligned), pack `MemoryFlushResponse`.

---

## Task 21 — `NonCoherentReceiverAllocation::invalidate_endpoint()` real implementation

Call real `vkInvalidateMappedMemoryRanges` first, then read the bytes from the receiver mapped pointer at each requested range, write them into the response payload, pack `MemoryInvalidateResponseHeader` + range entries.

---

## Task 22 — Phase 2 end-to-end loopback test

Extends Task 14 to:
1. Map + write bytes
2. Call `vkFlushMappedMemoryRanges` for the written range
3. Unmap
4. Assert the receiver-side memory now has the written bytes (NOT undefined as in Phase 1)

Plus the readback path:
1. Receiver-side writes (set up via fake driver or rapid-vulkan compute write)
2. Map (no fetch under N2)
3. Invalidate the range
4. Read the source staging
5. Assert it has the receiver-side values

**🛑 Phase 2 stop point — review before continuing to Phase 3a.**

---

# PHASE 3a — Coherent C2.1 map/unmap bracketed copies

## Task 23 — Wire format extension for coherent bracketed copies

**Files:**
- Modify: `src/vkfwd/ferry/core/memory_map/wire_format.hpp`

For coherent allocations the map RESPONSE carries the initial bytes (receiver → source fetch), and the unmap REQUEST carries the whole mapped range as bytes (source → receiver push). The unmap path's wire format already supports range_count > 0 from Phase 1 (Task 2's `MemoryTransferRange`). Extend MemoryMapResponse to carry an optional payload:

```cpp
struct MemoryMapResponse {
    std::uint32_t manager_revision  = kMemoryMapManagerRevision;
    std::int32_t  return_value      = VK_SUCCESS;
    VkDeviceSize  effective_size    = 0;
    // For coherent allocations, the response is followed by exactly
    // `effective_size` raw bytes that the forwarder copies into source
    // staging at [mapped_offset, mapped_offset + effective_size). Non-coherent
    // allocations leave this empty (the response stream stops at the struct).
    std::uint32_t initial_payload_present = 0;  // 0 = no payload, 1 = payload follows
    std::uint32_t pad0                    = 0;
};
```

---

## Task 24 — `CoherentForwarderAllocation::map()` real implementation

Identical to non-coherent except:
1. After unpacking the response header, if `initial_payload_present == 1`, copy `effective_size` bytes from the response stream (immediately after the response struct) into source staging at `[mapped_offset, mapped_offset + effective_size)`.

Failure paths same as non-coherent: VM failures release before sending; receiver rejections release on the response path.

---

## Task 25 — `CoherentForwarderAllocation::unmap()` real implementation

Identical to non-coherent except:
1. Build a MemoryUnmap chunk with `range_count = 1`, one `MemoryTransferRange { mapped_offset, mapped_size, payload_offset }`, followed by the raw bytes from source staging at `[mapped_offset, mapped_offset + mapped_size)`.

---

## Task 26 — `CoherentReceiverAllocation::map_endpoint()` real implementation

Same as non-coherent for the wire side, plus:
1. After calling real `vkMapMemory`, copy the receiver mapped range bytes into the response payload area
2. Set `initial_payload_present = 1`

---

## Task 27 — `CoherentReceiverAllocation::unmap_endpoint()` real implementation

Same as non-coherent for the wire side, plus:
1. Read the single MemoryTransferRange's payload from the request
2. Copy the bytes into the receiver mapped pointer at `(range.offset - mapped_offset)`
3. (No receiver-side `vkFlushMappedMemoryRanges` call — coherent memory does not need it)
4. Call real `vkUnmapMemory`

---

## Task 28 — Coherent flush/invalidate stay as `VK_ERROR_FEATURE_NOT_PRESENT`

For C2.1, `CoherentForwarderAllocation::flush()` / `invalidate()` remain placeholders returning `VK_ERROR_FEATURE_NOT_PRESENT`. Vulkan apps using coherent memory don't typically call flush/invalidate anyway (and the spec says it's a no-op for coherent memory — but vkfwd's protocol can't fulfill it without extra wire bytes). C2.3 (future Phase 3c) handles persistent coherent maps via sync-point copies; C2.1 covers the common case.

Document this explicitly in code comments.

---

## Task 29 — Phase 3a end-to-end loopback test

Allocate coherent host-visible memory, map, write bytes, unmap, assert receiver-side memory has those bytes. Then read-side: write receiver-side bytes (via compute or directly), map, assert source staging has the receiver-side values from before the map.

**🛑 Phase 3a stop point — review complete Phases 1+2+3a integration before merge.**

---

## Out-of-scope items (left for future plans)

- Phase 3b — C2.2 skip-copy-on-map flag (selection mechanism TBD; vkfwd-specific extension is the most likely option, but env var or config knob also viable)
- Phase 3c — C2.3 sync-point copies (requires `vkQueueSubmit`, fence/semaphore wait commands in the generator's `TARGET_COMMANDS`; large work)
- Phase 3d — C2.4 diff-based receiver→source transfer (wire-cost optimization layered on top of C2.3)
- Phase 4 — N4 page-protection dirty tracking, C3 hazard tracker, C4 source-side page-protection dirty tracking (all optional perf optimizations)
- Phase 5 — broader end-to-end coverage including real compute submits, descriptor sets, multiple concurrent mapped allocations

---

## Self-review checklist

- [ ] Every task lists the exact files it touches
- [ ] Every code-bearing step shows the actual code (no "TODO" / "fill in details")
- [ ] Type names used in later tasks match those defined in earlier tasks (`MemoryMapRequest`, `MemoryUnmapRequestHeader`, `MemoryTransferRange`, `dispatch_manual_command`, `source_to_receiver_memory`, `source_to_receiver_device`)
- [ ] Each phase ends at a fully working state with passing tests and a clear stop point
- [ ] Test policy is consistent: pure-CPU pack/unpack + forwarder/receiver unit tests in the existing `install_pack_unpack_transport` style, plus one end-to-end loopback test per phase
- [ ] Generator changes are clearly called out as "regenerate, then commit the regenerated files"
- [ ] Out-of-scope items are listed explicitly so a reader knows what's NOT happening here
