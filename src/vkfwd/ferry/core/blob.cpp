#include "blob.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <memory>
#include <new>

namespace vkfwd {
namespace {

constexpr std::size_t kMinimumChunkSize = 1;
constexpr std::size_t kDefaultChunkSize = 4096;

std::size_t checked_add(std::size_t lhs, std::size_t rhs) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) { throw std::bad_array_new_length(); }
    return lhs + rhs;
}

} // namespace

Blob::Blob(): Blob(kDefaultChunkSize) {}

Blob::Blob(std::size_t chunk_size): chunk_size_(std::max(chunk_size, kMinimumChunkSize)) {}

void Blob::reset() {
    chunks_.clear();
    size_ = 0;
}

SafeArrayView<std::uint8_t> Blob::grow(std::size_t size, std::size_t alignment) {
    alignment = normalize_alignment(alignment);
    if (size == 0) { return {}; }

    Chunk &     chunk     = ensure_chunk(size, alignment);
    void *      cursor    = chunk.data.get() + chunk.used;
    std::size_t available = chunk.allocation_size - chunk.used;
    void *      aligned   = std::align(alignment, size, cursor, available);
    if (!aligned) { throw std::bad_alloc(); }

    const auto        physical_offset = static_cast<std::size_t>(static_cast<std::byte *>(aligned) - chunk.data.get());
    const std::size_t logical_offset  = checked_add(chunk.logical_begin, physical_offset);
    chunk.used                        = checked_add(physical_offset, size);
    size_                             = checked_add(chunk.logical_begin, chunk.used);
    return SafeArrayView<std::uint8_t>(size, reinterpret_cast<std::uint8_t *>(aligned), logical_offset);
}

SafeArrayView<const std::uint8_t> Blob::data_at(std::size_t offset, std::size_t size) const {
    if (size == 0) { return {}; }
    for (const auto & chunk : chunks_) {
        if (offset < chunk.logical_begin) { continue; }
        const std::size_t local = offset - chunk.logical_begin;
        if (local <= chunk.used && size <= chunk.used - local) {
            return SafeArrayView<const std::uint8_t>(size, reinterpret_cast<const std::uint8_t *>(chunk.data.get() + local), offset);
        }
    }
    return {};
}

std::size_t Blob::normalize_alignment(std::size_t alignment) {
    if (alignment <= 1) { return 1; }
    if (std::has_single_bit(alignment)) { return alignment; }
    if (alignment > (std::size_t {1} << (std::numeric_limits<std::size_t>::digits - 1))) { throw std::bad_array_new_length(); }
    return std::bit_ceil(alignment);
}

Blob::Chunk Blob::allocate_chunk(std::size_t capacity, std::size_t alignment) {
    alignment = normalize_alignment(alignment);

    Chunk chunk;
    chunk.capacity  = std::max(capacity, kMinimumChunkSize);
    chunk.alignment = alignment;
    // Chunks include alignment padding because the arena may need to return a
    // stable aligned view from the middle of byte storage. The exposed capacity is
    // the payload budget; allocation_size is the private slack needed to honor the
    // next grow request without changing the view lifetime contract.
    chunk.allocation_size = checked_add(chunk.capacity, alignment - 1);
    chunk.data            = std::make_unique<std::byte[]>(chunk.allocation_size);
    return chunk;
}

Blob::Chunk & Blob::ensure_chunk(std::size_t size, std::size_t alignment) {
    if (!chunks_.empty()) {
        Chunk & chunk = chunks_.back();
        if (chunk.alignment >= alignment) {
            void *      cursor    = chunk.data.get() + chunk.used;
            std::size_t available = chunk.allocation_size - chunk.used;
            if (std::align(alignment, size, cursor, available)) { return chunk; }
        }
    }

    const std::size_t capacity = std::max(chunk_size_, size);
    chunks_.push_back(allocate_chunk(capacity, alignment));
    chunks_.back().logical_begin = size_;
    return chunks_.back();
}

} // namespace vkfwd
