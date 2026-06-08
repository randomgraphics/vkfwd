// Windows implementation of vkfwd::memory_map::vm. The entire body is guarded
// by _WIN32 so this TU compiles to an empty object on Linux/macOS.

#if defined(_WIN32)

    // Keep the windows.h surface area minimal — we only need VirtualAlloc and
    // GetSystemInfo, and unrelated macros (min/max, etc.) collide with C++ code.
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include "memory_map/vm_primitives.hpp"

    #include <windows.h>

    #include <atomic>
    #include <cstddef>

namespace vkfwd::memory_map::vm {

namespace {

// We report dwAllocationGranularity (typically 64 KiB), not dwPageSize (4 KiB),
// because VirtualAlloc rounds a reservation's base address to allocation
// granularity. If we reported 4 KiB and the caller used page_floor() to
// compute a commit offset relative to the reservation base, mid-granularity
// offsets would still be valid for VirtualAlloc(MEM_COMMIT), but a downstream
// caller assuming `base % host_page_size() == 0` would be wrong. Using the
// stricter value keeps the alignment contract honest on every platform.
std::atomic<std::size_t> g_page_size {0};

} // namespace

std::size_t host_page_size() {
    std::size_t cached = g_page_size.load(std::memory_order_relaxed);
    if (cached != 0) return cached;
    SYSTEM_INFO info {};
    ::GetSystemInfo(&info);
    cached = static_cast<std::size_t>(info.dwAllocationGranularity);
    g_page_size.store(cached, std::memory_order_relaxed);
    return cached;
}

void * reserve(std::size_t size) {
    // Tests may inject a one-shot failure to exercise OOM cleanup paths that
    // the real kernel would only produce under address-space exhaustion.
    if (auto hook = g_test_reserve_failure_hook) {
        g_test_reserve_failure_hook = nullptr;
        return hook(size);
    }
    // MEM_RESERVE + PAGE_NOACCESS: claim VA without charging the system commit
    // limit. A subsequent MEM_COMMIT on a sub-range charges only that
    // sub-range, which is exactly the "reserve allocation_size up front, commit
    // mapped sub-range on map" pattern NonCoherentForwarderAllocation needs.
    return ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
}

bool commit(void * base, std::size_t size) {
    if (auto hook = g_test_commit_failure_hook) {
        g_test_commit_failure_hook = nullptr;
        return hook(base, size);
    }
    // MEM_COMMIT on an already-reserved range. Windows allows the commit
    // address to be page-aligned (4 KiB) even when the original reservation
    // was rounded to allocation granularity. Returns the base of the committed
    // range on success, NULL on failure.
    return ::VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

void release(void * base, std::size_t size) {
    // MEM_RELEASE requires the size argument to be zero and releases the entire
    // reservation that begins at `base`. The size parameter on our wrapper is
    // kept for POSIX symmetry; we deliberately ignore it here.
    (void) size;
    ::VirtualFree(base, 0, MEM_RELEASE);
}

} // namespace vkfwd::memory_map::vm

#endif
