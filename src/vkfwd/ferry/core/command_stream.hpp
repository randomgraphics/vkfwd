#pragma once

#include "logging.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace vkfwd {

template<class T>
concept TriviallyCopyable = std::is_trivially_copyable_v<std::remove_cv_t<T>>;

template<TriviallyCopyable T>
class SafeArrayView {
public:
    SafeArrayView() = default;
    SafeArrayView(std::size_t count, T * ptr): count_(count), ptr_(ptr) {}

    bool        empty() const { return count_ == 0; }
    std::size_t size() const { return count_; }

    T * begin() const { return ptr_; }
    T * end() const { return ptr_ ? ptr_ + count_ : nullptr; }

    // Views intentionally expose element access through at() instead of a raw
    // base pointer. Callers that need to reinterpret a complete CommandStream
    // range must first hold a non-empty view and then take the address of a
    // checked element.
    T & at(std::size_t index) const {
        static std::remove_const_t<T> dummy {};
        if (!ptr_ || index >= count_) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd command stream view access out of range, index={}, count={}", index, count_);
            return dummy;
        }
        return ptr_[index];
    }

    // Returns the address of an element only after the same bounds check used
    // by at(). This is the preferred way to hand a stream-backed object to code
    // that needs pointer syntax without copying the stored bytes.
    T * address(std::size_t index = 0) const {
        if (!ptr_ || index >= count_) [[unlikely]] {
            VKFWD_LOG_ERROR("vkfwd command stream view address out of range, index={}, count={}", index, count_);
            return nullptr;
        }
        return ptr_ + index;
    }

    bool set(std::size_t index, const T & value)
        requires(!std::is_const_v<T>)
    {
        if (!ptr_ || index >= count_) { return false; }
        std::memcpy(ptr_ + index, &value, sizeof(T));
        return true;
    }

    // Copies into the view while clamping to the owned allocation. This keeps
    // generated pack code from turning an inconsistent Vulkan count/pointer pair
    // into an arena overrun; the returned count tells callers whether the source
    // was truncated by the destination boundary.
    std::size_t set(std::size_t offset, std::size_t count, const T * data)
        requires(!std::is_const_v<T>)
    {
        if (!ptr_ || !data || offset >= count_ || count == 0) { return 0; }
        const std::size_t writable = std::min(count, count_ - offset);
        std::memcpy(ptr_ + offset, data, writable * sizeof(T));
        return writable;
    }

    // Reinterprets the same bytes as another copyable payload view. Any tail bytes
    // that do not form a complete T2 element are intentionally dropped so generated
    // serializers never expose a partially addressable object.
    template<TriviallyCopyable T2>
    SafeArrayView<T2> cast() const {
        static_assert(!std::is_const_v<T> || std::is_const_v<T2>, "cannot cast a const SafeArrayView to a mutable view");
        const std::size_t byte_count = count_ * sizeof(T);
        return SafeArrayView<T2>(byte_count / sizeof(T2), reinterpret_cast<T2 *>(ptr_));
    }

private:
    std::size_t count_ = 0;
    T *         ptr_   = nullptr;
};

// A growable command byte stream that owns copied payload bytes in stable
// chunks. Shallow command/structure payloads and sized arrays are allocated as
// one contiguous range; pointer targets may move to later chunks and encode
// field-relative offsets through any explicit gap records in the stream.
class CommandStream {
public:
    static constexpr std::size_t kBaseAlignment    = 128;
    static constexpr std::size_t kMinimumChunkSize = 1024;
    static constexpr std::size_t kDefaultChunkSize = 4096;

    CommandStream();
    explicit CommandStream(std::size_t chunk_size);
    CommandStream(const CommandStream &)                 = delete;
    CommandStream & operator=(const CommandStream &)     = delete;
    CommandStream(CommandStream &&) noexcept             = default;
    CommandStream & operator=(CommandStream &&) noexcept = default;

    // Reset only clears storage. Request-stream framing is a protocol concern
    // written explicitly by Forwarder so response streams and serialization
    // scratch arenas never acquire a routing header by accident.
    void        reset();
    std::size_t size() const { return size_; }
    // Reports whether the current logical byte stream lives in a single backing
    // allocation. Consumers may use this to decide whether one whole-stream range
    // can be inspected without stitching chunks together; it is not an allocation
    // policy knob.
    bool is_contiguous() const { return chunks_.size() <= 1; }

    // Copies the current logical stream into one backing allocation. Transport
    // implementations use this when their framing layer needs a single byte span
    // but command packers are still free to grow this stream in multiple chunks.
    CommandStream flatten() const;

    // Grows the arena and returns a bounded view over the newly appended object
    // bytes. Alignment is a logical stream rule as well as a physical allocation
    // rule: the returned offset is aligned so flattening into another
    // kBaseAlignment-backed CommandStream preserves typed pointer validity.
    SafeArrayView<std::uint8_t> grow(std::size_t size, std::size_t alignment = 1, std::size_t * offset = nullptr);

    template<TriviallyCopyable T>
    SafeArrayView<T> grow(std::size_t count, std::size_t alignment = alignof(T), std::size_t * offset = nullptr) {
        static_assert(kBaseAlignment % alignof(T) == 0, "CommandStream base alignment must satisfy every typed payload alignment");
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) { throw std::bad_array_new_length(); }
        return grow(count * sizeof(T), alignment, offset).template cast<T>();
    }

    template<TriviallyCopyable T>
    T & grow() {
        auto v = grow<T>(1);
        return v.at(0);
    }

    SafeArrayView<const std::uint8_t> at(std::size_t offsetInBytes, std::size_t sizeInBytes) const;
    SafeArrayView<std::uint8_t>       at(std::size_t offsetInBytes, std::size_t sizeInBytes);

    template<TriviallyCopyable T>
    SafeArrayView<const T> at(std::size_t offsetInBytes, std::size_t sizeInBytes = sizeof(T)) const {
        // Typed access is only valid at naturally aligned stream offsets. Gap
        // records or other unaligned protocol bytes must be read as bytes and
        // copied into a local object before interpretation.
        constexpr size_t align_of_t = alignof(T);
        constexpr size_t size_of_t  = sizeof(T);
        if (!is_aligned(offsetInBytes, align_of_t) || sizeInBytes < size_of_t || (sizeInBytes % size_of_t) != 0) { return {}; }
        return at(offsetInBytes, sizeInBytes).cast<const T>();
    }

    template<TriviallyCopyable T>
    SafeArrayView<T> at(std::size_t offsetInBytes, std::size_t sizeInBytes = sizeof(T)) {
        // Mutable unpackers repair pointer fields in-place, but they still have
        // to prove that the entire typed payload is backed by one CommandStream chunk
        // before exposing a writable view.
        constexpr size_t align_of_t = alignof(T);
        constexpr size_t size_of_t  = sizeof(T);
        if (!is_aligned(offsetInBytes, align_of_t) || sizeInBytes < size_of_t || (sizeInBytes % size_of_t) != 0) { return {}; }
        return at(offsetInBytes, sizeInBytes).cast<T>();
    }

private:
    struct Chunk {
        struct AlignedDeleter {
            void operator()(std::byte * ptr) const noexcept;
        };

        std::unique_ptr<std::byte[], AlignedDeleter> data;
        std::size_t                                  logical_begin = 0;
        std::size_t                                  capacity      = 0;
        std::size_t                                  used          = 0;
    };

    static std::size_t normalize_alignment(std::size_t alignment);
    static std::size_t align_up(std::size_t value, std::size_t alignment);
    static bool        is_aligned(std::size_t value, std::size_t alignment) { return (value % alignment) == 0; }
    static Chunk       allocate_chunk(std::size_t capacity);
    Chunk &            ensure_chunk(std::size_t logical_offset, std::size_t size);
    void               close_current_chunk();

    std::size_t        chunk_size_ = 0;
    std::size_t        size_       = 0;
    std::vector<Chunk> chunks_;
};

} // namespace vkfwd
