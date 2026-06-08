// macOS implementation of vkfwd::memory_map::vm. The entire body is guarded by
// __APPLE__ so this TU compiles to an empty object on Linux/Windows.

#if defined(__APPLE__)

    #include "memory_map/vm_primitives.hpp"

    #include <sys/mman.h>
    #include <unistd.h>

    #include <atomic>
    #include <cstddef>

namespace vkfwd::memory_map::vm {

namespace {

// Cached on first use. Apple Silicon reports 16384; Intel macOS reports 4096.
// The reservation base is implicitly aligned to that granularity by the
// kernel, so the value here also defines our page_floor/page_ceil rounding.
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
    // No MAP_NORESERVE on macOS: the BSD VM layer already treats a PROT_NONE
    // anonymous mapping as a pure VA reservation that does not charge against
    // physical memory until pages are mapped RW and touched. So MAP_PRIVATE |
    // MAP_ANONYMOUS with PROT_NONE gives us the same "reserve without commit"
    // behavior we get from MAP_NORESERVE on Linux.
    void * p = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
}

bool commit(void * base, std::size_t size) {
    // mprotect flips the previously-reserved PROT_NONE range to RW. Pages are
    // demand-zero-filled on first touch, so RSS only grows as the source
    // application actually writes through the mapping.
    return ::mprotect(base, size, PROT_READ | PROT_WRITE) == 0;
}

void release(void * base, std::size_t size) { ::munmap(base, size); }

} // namespace vkfwd::memory_map::vm

#endif
