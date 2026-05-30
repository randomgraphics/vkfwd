#pragma once

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
    SafeArrayView(std::size_t count, T * ptr, std::size_t offset = 0): count_(count), ptr_(ptr), offset_(offset) {}

    bool        empty() const { return count_ == 0; }
    T *         data() const { return ptr_; }
    std::size_t size() const { return count_; }
    std::size_t offset() const { return offset_; }

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
        return SafeArrayView<T2>(byte_count / sizeof(T2), reinterpret_cast<T2 *>(ptr_), offset_);
    }

private:
    std::size_t count_  = 0;
    T *         ptr_    = nullptr;
    std::size_t offset_ = 0;
};

// A growable logical byte stream that owns copied payload bytes in stable
// chunks. Offsets returned by append/grow are relative to the beginning of this
// logical stream, not to a particular chunk allocation. That lets command and
// structure packers replace process-local pointers with moveable offsets while
// still allowing Blob to split storage internally.
class Blob {
public:
    Blob();
    explicit Blob(std::size_t chunk_size);
    Blob(const Blob &)                 = delete;
    Blob & operator=(const Blob &)     = delete;
    Blob(Blob &&) noexcept             = default;
    Blob & operator=(Blob &&) noexcept = default;

    void        reset();
    std::size_t size() const { return size_; }
    std::size_t chunk_size() const { return chunk_size_; }

    // Grows the arena and returns a bounded view over exactly the new allocation.
    // The returned memory is uninitialized so callers can avoid redundant clears
    // when immediately serializing Vulkan payload bytes into it.
    SafeArrayView<std::uint8_t> grow(std::size_t size, std::size_t alignment = 1);
    SafeArrayView<const std::uint8_t> data_at(std::size_t offset, std::size_t size) const;

    template<TriviallyCopyable T>
    SafeArrayView<T> grow(std::size_t count, std::size_t alignment = alignof(T)) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) { throw std::bad_array_new_length(); }
        return grow(count * sizeof(T), std::max(alignment, alignof(T))).template cast<T>();
    }

private:
    struct Chunk {
        std::unique_ptr<std::byte[]> data;
        std::size_t                  logical_begin   = 0;
        std::size_t                  capacity        = 0;
        std::size_t                  allocation_size = 0;
        std::size_t                  used            = 0;
        std::size_t                  alignment       = 1;
    };

    static std::size_t normalize_alignment(std::size_t alignment);
    static Chunk       allocate_chunk(std::size_t capacity, std::size_t alignment);
    Chunk &            ensure_chunk(std::size_t size, std::size_t alignment);

    std::size_t        chunk_size_ = 0;
    std::size_t        size_       = 0;
    std::vector<Chunk> chunks_;
};

} // namespace vkfwd
