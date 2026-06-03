#include "command_stream.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>

namespace vkfwd {
namespace {

constexpr std::size_t kDefaultChunkSize = 4096;

std::size_t checked_add(std::size_t lhs, std::size_t rhs) {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) { throw std::bad_array_new_length(); }
    return lhs + rhs;
}

} // namespace

CommandStream::CommandStream(): CommandStream(kDefaultChunkSize) {}

CommandStream::CommandStream(std::size_t chunk_size): chunk_size_(std::max(chunk_size, kMinimumChunkSize)) {}

void CommandStream::reset() {
    chunks_.clear();
    size_ = 0;
    if (has_stream_header_) { write_stream_header(); }
}

void CommandStream::reset(StreamId stream_id) {
    has_stream_header_ = true;
    stream_id_         = stream_id;
    reset();
}

void CommandStream::Chunk::AlignedDeleter::operator()(std::byte * ptr) const noexcept {
    ::operator delete[](ptr, std::align_val_t {CommandStream::kBaseAlignment});
}

SafeArrayView<std::uint8_t> CommandStream::grow(std::size_t size, std::size_t alignment, std::size_t * offset) {
    alignment = normalize_alignment(alignment);
    if (size == 0) { return {}; }
    if (kBaseAlignment % alignment != 0) { throw std::bad_array_new_length(); }

    std::size_t logical_offset = align_up(size_, alignment);
    if (!chunks_.empty()) {
        const Chunk & chunk = chunks_.back();
        if (logical_offset >= chunk.logical_begin) {
            const std::size_t local = logical_offset - chunk.logical_begin;
            if (local <= chunk.capacity) {
                const std::size_t available           = chunk.capacity - local;
                const bool        fits                = size <= available;
                const std::size_t leftover            = fits ? available - size : 0;
                const bool        has_tag_space_after = leftover == 0 || leftover >= sizeof(CommandStreamGapHeader);
                if (!fits || !has_tag_space_after) {
                    close_current_chunk();
                    logical_offset = align_up(size_, alignment);
                }
            }
        }
    }

    Chunk &           chunk           = ensure_chunk(logical_offset, size);
    const std::size_t physical_offset = logical_offset - chunk.logical_begin;
    if (physical_offset > chunk.used) {
        // Alignment padding is not a parsing sentinel, but it is still copied by
        // flatten(). Initialize it so transports never expose stale arena bytes.
        std::fill(chunk.data.get() + chunk.used, chunk.data.get() + physical_offset, std::byte {0});
    }
    chunk.used = checked_add(physical_offset, size);
    size_      = checked_add(logical_offset, size);
    if (offset) { *offset = logical_offset; }
    return SafeArrayView<std::uint8_t>(size, reinterpret_cast<std::uint8_t *>(chunk.data.get() + physical_offset));
}

SafeArrayView<const std::uint8_t> CommandStream::at(std::size_t offset, std::size_t size) const {
    if (size == 0) { return {}; }
    for (const auto & chunk : chunks_) {
        if (offset < chunk.logical_begin) { continue; }
        const std::size_t local = offset - chunk.logical_begin;
        if (local <= chunk.used && size <= chunk.used - local) {
            return SafeArrayView<const std::uint8_t>(size, reinterpret_cast<const std::uint8_t *>(chunk.data.get() + local));
        }
    }
    return {};
}

SafeArrayView<std::uint8_t> CommandStream::at(std::size_t offset, std::size_t size) {
    if (size == 0) { return {}; }
    for (auto & chunk : chunks_) {
        if (offset < chunk.logical_begin) { continue; }
        const std::size_t local = offset - chunk.logical_begin;
        if (local <= chunk.used && size <= chunk.used - local) {
            return SafeArrayView<std::uint8_t>(size, reinterpret_cast<std::uint8_t *>(chunk.data.get() + local));
        }
    }
    return {};
}

CommandStream CommandStream::flatten() const {
    CommandStream flattened(size_ == 0 ? kMinimumChunkSize : size_);
    if (size_ == 0) { return flattened; }

    auto destination = flattened.grow(size_, 1);
    for (const auto & chunk : chunks_) {
        if (chunk.used == 0) { continue; }
        const auto * source = reinterpret_cast<const std::uint8_t *>(chunk.data.get());
        if (destination.set(chunk.logical_begin, chunk.used, source) != chunk.used) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd command stream flatten failed: could not copy chunk, logical_begin={}, used={}, stream_size={}", chunk.logical_begin,
                            chunk.used, size_);
            return {};
        }
    }
    return flattened;
}

CommandStream::Chunk CommandStream::allocate_chunk(std::size_t capacity) {
    Chunk chunk;
    chunk.capacity = align_up(std::max(capacity, kMinimumChunkSize), kBaseAlignment);
    // Every chunk starts at the protocol's fixed base alignment. Combined with
    // grow() aligning logical offsets, a flattened stream can still expose typed
    // Vulkan structs directly without copying them into staging storage.
    chunk.data.reset(static_cast<std::byte *>(::operator new[](chunk.capacity, std::align_val_t {kBaseAlignment})));
    return chunk;
}

std::size_t CommandStream::normalize_alignment(std::size_t alignment) {
    if (alignment <= 1) { return 1; }
    if (std::has_single_bit(alignment)) { return alignment; }
    if (alignment > kBaseAlignment) { throw std::bad_array_new_length(); }
    return std::bit_ceil(alignment);
}

std::size_t CommandStream::align_up(std::size_t value, std::size_t alignment) {
    alignment                   = normalize_alignment(alignment);
    const std::size_t remainder = value % alignment;
    if (remainder == 0) { return value; }
    return checked_add(value, alignment - remainder);
}

CommandStream::Chunk & CommandStream::ensure_chunk(std::size_t logical_offset, std::size_t size) {
    if (!chunks_.empty()) {
        Chunk & chunk = chunks_.back();
        if (logical_offset >= chunk.logical_begin) {
            const std::size_t local = logical_offset - chunk.logical_begin;
            if (local <= chunk.capacity && size <= chunk.capacity - local) { return chunk; }
        }
    }

    const std::size_t capacity = std::max(chunk_size_, checked_add(size, sizeof(CommandStreamGapHeader)));
    chunks_.push_back(allocate_chunk(capacity));
    chunks_.back().logical_begin = logical_offset;
    return chunks_.back();
}

void CommandStream::close_current_chunk() {
    if (chunks_.empty()) { return; }

    Chunk & chunk = chunks_.back();
    if (chunk.used == chunk.capacity) {
        size_ = checked_add(chunk.logical_begin, chunk.used);
        return;
    }

    const std::size_t remaining = chunk.capacity - chunk.used;
    if (remaining < sizeof(CommandStreamGapHeader)) { throw std::bad_array_new_length(); }

    if (remaining > std::numeric_limits<std::uint32_t>::max()) { throw std::bad_array_new_length(); }
    CommandStreamGapHeader header {
        .magic = kCommandStreamGapMagic,
        .size  = static_cast<std::uint32_t>(remaining),
    };

    std::memcpy(chunk.data.get() + chunk.used, &header, sizeof(header));
    if (remaining > sizeof(header)) {
        // The gap payload is semantically dead weight, but initializing it keeps
        // transports from copying stale arena contents into flattened streams.
        std::fill(chunk.data.get() + chunk.used + sizeof(header), chunk.data.get() + chunk.capacity, std::byte {0});
    }
    chunk.used = chunk.capacity;
    size_      = checked_add(chunk.logical_begin, chunk.used);
}

void CommandStream::write_stream_header() {
    StreamHeader header {
        .stream_id = stream_id_,
    };
    // Request-stream framing is stored as ordinary stream-owned bytes so
    // flattening, transport copying, and receiver parsing all observe exactly
    // the same base-aligned envelope.
    auto destination = grow<StreamHeader>(1, kBaseAlignment);
    destination.set(0, header);
}

} // namespace vkfwd
