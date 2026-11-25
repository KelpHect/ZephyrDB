#include "zephyr/core/arena.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace zephyr::core {

Arena::Arena(size_t slab_bytes)
    : default_slab_bytes_(slab_bytes < 64 ? 64 : slab_bytes) {}

Arena::~Arena() {
    free_all_slabs();
}

Arena::Arena(Arena&& other) noexcept
    : default_slab_bytes_(other.default_slab_bytes_),
      head_(other.head_),
      current_(other.current_),
      bytes_used_(other.bytes_used_),
      slab_count_(other.slab_count_) {
    other.head_ = nullptr;
    other.current_ = nullptr;
    other.bytes_used_ = 0;
    other.slab_count_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        free_all_slabs();
        default_slab_bytes_ = other.default_slab_bytes_;
        head_ = other.head_;
        current_ = other.current_;
        bytes_used_ = other.bytes_used_;
        slab_count_ = other.slab_count_;

        other.head_ = nullptr;
        other.current_ = nullptr;
        other.bytes_used_ = 0;
        other.slab_count_ = 0;
    }
    return *this;
}

Arena::Slab* Arena::allocate_slab(size_t min_capacity) {
    const size_t cap = std::max(default_slab_bytes_, min_capacity);
    void* raw = ::operator new(sizeof(Slab) + cap, std::align_val_t{64});
    Slab* slab = ::new (raw) Slab{};
    slab->next = nullptr;
    slab->capacity = cap;
    slab->offset = 0;

    if (!head_) {
        head_ = slab;
        current_ = slab;
    } else {
        Slab* tail = current_ ? current_ : head_;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = slab;
        current_ = slab;
    }
    ++slab_count_;
    return slab;
}

void Arena::free_all_slabs() noexcept {
    Slab* cur = head_;
    while (cur) {
        Slab* next = cur->next;
        cur->~Slab();
        ::operator delete(cur, std::align_val_t{64});
        cur = next;
    }
    head_ = nullptr;
    current_ = nullptr;
    bytes_used_ = 0;
    slab_count_ = 0;
}

void* Arena::alloc_bytes(size_t size, size_t align) {
    if (size == 0) {
        size = 1;
    }
    if (align == 0) {
        align = 1;
    } else {
        align = std::bit_ceil(align);
    }

    while (current_) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(current_->data());
        const uintptr_t cur_addr = base + current_->offset;
        const uintptr_t aligned_addr = (cur_addr + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
        const size_t padding = static_cast<size_t>(aligned_addr - cur_addr);
        const size_t needed = padding + size;

        if (current_->offset + needed <= current_->capacity) {
            current_->offset += needed;
            bytes_used_ += size;
            return reinterpret_cast<void*>(aligned_addr);
        }

        if (current_->next) {
            current_ = current_->next;
        } else {
            break;
        }
    }

    // Allocate a new slab large enough for the requested size + worst-case alignment padding.
    Slab* slab = allocate_slab(size + align);
    const uintptr_t base = reinterpret_cast<uintptr_t>(slab->data());
    const uintptr_t aligned_addr = (base + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    const size_t padding = static_cast<size_t>(aligned_addr - base);
    slab->offset = padding + size;
    bytes_used_ += size;
    return reinterpret_cast<void*>(aligned_addr);
}

const char* Arena::dup_cstr(std::string_view sv) {
    char* buf = static_cast<char*>(alloc_bytes(sv.size() + 1, 1));
    if (!sv.empty()) {
        std::memcpy(buf, sv.data(), sv.size());
    }
    buf[sv.size()] = '\0';
    return buf;
}

const uint8_t* Arena::dup_bytes(const void* data, size_t len) {
    if (len == 0 || !data) {
        return nullptr;
    }
    uint8_t* buf = static_cast<uint8_t*>(alloc_bytes(len, 8));
    std::memcpy(buf, data, len);
    return buf;
}

void Arena::reset() noexcept {
    for (Slab* s = head_; s != nullptr; s = s->next) {
        s->offset = 0;
    }
    current_ = head_;
    bytes_used_ = 0;
}

} // namespace zephyr::core
