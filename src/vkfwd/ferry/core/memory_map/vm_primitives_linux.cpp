// Linux/Android implementation of vkfwd::memory_map::vm. The entire body is
// guarded by __linux__ so this TU compiles to an empty object on macOS/Windows
// and CMake can list all three platform sources unconditionally.

#if defined(__linux__)

    #include "memory_map/vm_primitives.hpp"

    #include <sys/mman.h>
    #include <unistd.h>

    #include <atomic>
    #include <cstddef>

namespace vkfwd::memory_map::vm {

namespace {

// Cached on first use. Atomic so racing callers either see 0 (and recompute
// the same constant from sysconf) or the cached value — both are safe.
std::atomic<std::size_t> g_page_size {0};

} // namespace

std::size_t host_page_size() {
    std::size_t cached = g_page_size.load(std::memory_order_relaxed);
    if (cached != 0) return cached;
    cached = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    g_page_size.store(cached, std::memory_order_relaxed);
    return cached;
}

void * reserve(std::size_t size) {
    // MAP_NORESERVE: do not charge the reservation against the OS commit limit
    // (/proc/sys/vm/overcommit_memory). A 4 GiB PROT_NONE reservation here
    // must cost ~0 swap; only the mprotect'd sub-range counts toward RSS.
    // PROT_NONE: any access faults until commit() flips it to RW, which makes
    // unintended early writes into a fault rather than silent corruption.
    void * p = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
}

bool commit(void * base, std::size_t size) {
    // Linux has no separate "commit" primitive: mprotect flips an already-
    // reserved range from PROT_NONE to RW. Pages are demand-paged on first
    // touch, so the commit limit only takes the hit as pages are actually
    // written (because the reservation was MAP_NORESERVE).
    return ::mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void release(void * base, std::size_t size) {
    // munmap drops the entire reservation in one call regardless of which
    // sub-pages were committed.
    ::munmap(base, size);
}

} // namespace vkfwd::memory_map::vm

#endif
