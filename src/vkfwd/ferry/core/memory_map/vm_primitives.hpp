#pragma once

#include <cstddef>

// Single-header platform abstraction for the source-side VM reserve/commit/release
// primitive used by NonCoherentForwarderAllocation::map() (and later coherent paths).
//
// Why a custom primitive rather than plain malloc:
// - Vulkan's vkMapMemory contract requires `*ppData - offset` to be aligned to
//   VkPhysicalDeviceLimits::minMemoryMapAlignment. Reserving an entire allocation's
//   VA range at a page-aligned base satisfies that contract for free (the design
//   doc asserts min_memory_map_alignment <= host_page_size at allocate time).
// - A 4 GiB Vulkan allocation can be backed by ~4 GiB of source VA but ~0 RSS
//   until the mapped sub-range is committed. PROT_NONE + MAP_NORESERVE on Linux
//   keeps the reservation off the OS commit limit; MEM_RESERVE on Windows does
//   the same. Plain malloc would charge the full size to RSS up front.
// - This wrapper is the only place these platform syscalls are named, so the
//   rest of memory_map/ stays portable.
//
// host_page_size() returns the cached OS page granularity:
//   - 4096  on x86 Linux / Windows page size (but see below for VirtualAlloc)
//   - 16384 on Apple Silicon (ARM64 macOS uses 16 KiB pages)
//   - 65536 on Windows when reserving — VirtualAlloc rounds reservation base
//     to dwAllocationGranularity, which is the conservative value to use.
// page_floor / page_ceil round to host_page_size in either direction.

namespace vkfwd::memory_map::vm {

// Reserve `size` bytes of virtual address space without committing physical
// memory. Returns a page-aligned base pointer, or nullptr on failure. The
// returned range is PROT_NONE / PAGE_NOACCESS — touching it before a matching
// commit() faults. Pair every successful reserve() with exactly one release()
// using the same base+size.
void * reserve(std::size_t size);

// Commit a page-aligned sub-range of an existing reservation as readable and
// writable. `base` must lie within a prior reservation; `base` and `size` must
// be page-aligned (use page_floor / page_ceil). Returns false if the OS
// rejects the commit (e.g., out of physical memory). Idempotent: re-committing
// an already-committed page is a no-op on all supported platforms.
bool commit(void * base, std::size_t size);

// Release the entire reservation rooted at `base`. On Windows the OS demands
// size == 0 with MEM_RELEASE; we pass the size for symmetry with the POSIX
// munmap signature and ignore it on the Windows path.
void release(void * base, std::size_t size);

// OS page granularity, cached on first call. Matches what reserve() will use
// to align bases and what commit() requires for its sub-range arguments.
std::size_t host_page_size();

// Round `offset` down to the nearest multiple of host_page_size(). Used to
// compute the page-aligned base address of a mapped sub-range so the OS-level
// commit covers the requested user offset.
inline std::size_t page_floor(std::size_t offset) {
    const std::size_t page = host_page_size();
    return offset & ~(page - 1);
}

// Round `offset` up to the nearest multiple of host_page_size(). Used to
// compute the page-aligned end of a mapped sub-range so the commit covers the
// full requested size even when offset+size lands mid-page.
inline std::size_t page_ceil(std::size_t offset) {
    const std::size_t page = host_page_size();
    return (offset + page - 1) & ~(page - 1);
}

// Test-only failure injection. Production code must leave these null. When
// set, the next matching call observes the hook and returns the injected
// failure result; the slot is consumed (reset to nullptr) on use so each
// injection covers exactly one call. The reserve hook receives the request
// size; the commit hook receives base+size. Both must NOT perform the real
// OS operation — they exist purely to simulate kernel-level failure paths
// that are otherwise unreachable from unit tests.
inline void * (*g_test_reserve_failure_hook)(std::size_t)      = nullptr;
inline bool (*g_test_commit_failure_hook)(void *, std::size_t) = nullptr;

} // namespace vkfwd::memory_map::vm
