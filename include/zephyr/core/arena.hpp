#ifndef ZEPHYR_CORE_ARENA_HPP
#define ZEPHYR_CORE_ARENA_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace zephyr::core {

/// 64-byte aligned bump-pointer allocator using chained slabs.
/// Intended for fast per-query / per-transaction allocations with bulk reset/free.
class Arena {
public:
    explicit Arena(size_t slab_bytes = 65536);
    ~Arena();

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept;
    Arena& operator=(Arena&& other) noexcept;

    /// Allocate `size` bytes aligned to `align` (must be a power of two; clamped to >= 1).
    void* alloc_bytes(size_t size, size_t align = 8);

    /// Construct an object of type T in-place inside the arena.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = alloc_bytes(sizeof(T), alignof(T));
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    /// Allocate and default-construct an array of `count` elements of type T.
    template <typename T>
    T* alloc_array(size_t count) {
        if (count == 0) {
            return nullptr;
        }
        void* mem = alloc_bytes(sizeof(T) * count, alignof(T));
        T* arr = static_cast<T*>(mem);
        if constexpr (!std::is_trivially_default_constructible_v<T>) {
            std::uninitialized_default_construct_n(arr, count);
        }
        return arr;
    }

    /// Duplicate a string_view as a null-terminated C string inside the arena.
    const char* dup_cstr(std::string_view sv);

    /// Duplicate raw bytes inside the arena. Returns nullptr if len == 0.
    const uint8_t* dup_bytes(const void* data, size_t len);

    /// Reset all slabs for reuse without releasing memory back to the OS.
    void reset() noexcept;

    size_t bytes_used() const noexcept { return bytes_used_; }
    size_t slab_count() const noexcept { return slab_count_; }

private:
    struct alignas(64) Slab {
        Slab* next = nullptr;
        size_t capacity = 0;
        size_t offset = 0;

        uint8_t* data() noexcept {
            return reinterpret_cast<uint8_t*>(this) + sizeof(Slab);
        }
    };

    Slab* allocate_slab(size_t min_capacity);
    void free_all_slabs() noexcept;

    size_t default_slab_bytes_ = 65536;
    Slab* head_ = nullptr;
    Slab* current_ = nullptr;
    size_t bytes_used_ = 0;
    size_t slab_count_ = 0;
};

} // namespace zephyr::core

#endif // ZEPHYR_CORE_ARENA_HPP
