#include "zephyr/sql/simd_eval.hpp"

#include <algorithm>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace zephyr::sql {

namespace {

inline uint32_t ctz64(uint64_t x) noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return static_cast<uint32_t>(idx);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctzll(x));
#else
    uint32_t n = 0;
    while ((x & 1ULL) == 0) {
        x >>= 1;
        ++n;
    }
    return n;
#endif
}

template <typename T, typename ChunkFn>
std::vector<uint32_t> filter_column_impl(
    const T* data,
    size_t count,
    T rhs,
    CmpOp op,
    const uint8_t* nulls,
    const uint8_t* live,
    ChunkFn&& chunk_fn
) {
    std::vector<uint32_t> out;
    if (!data || count == 0) return out;

    for (size_t base = 0; base < count; base += 64) {
        size_t chunk_len = std::min<size_t>(64, count - base);
        uint64_t mask = chunk_fn(data + base, chunk_len, rhs, op);
        if (chunk_len < 64) {
            mask &= ((1ULL << chunk_len) - 1ULL);
        }
        while (mask != 0) {
            uint32_t bit = ctz64(mask);
            mask &= (mask - 1ULL);
            size_t row_idx = base + bit;
            if (nulls && nulls[row_idx] != 0) continue;
            if (live && live[row_idx] == 0) continue;
            out.push_back(static_cast<uint32_t>(row_idx));
        }
    }
    return out;
}

} // namespace

std::vector<uint32_t> filter_column_i64(
    const int64_t* data,
    size_t count,
    int64_t rhs,
    CmpOp op,
    const uint8_t* nulls,
    const uint8_t* live
) {
    return filter_column_impl<int64_t>(
        data, count, rhs, op, nulls, live,
        [](const int64_t* p, size_t n, int64_t v, CmpOp c) noexcept {
            return zephyr::cpp26::simd::filter_chunk_i64(p, n, v, c);
        }
    );
}

std::vector<uint32_t> filter_column_f64(
    const double* data,
    size_t count,
    double rhs,
    CmpOp op,
    const uint8_t* nulls,
    const uint8_t* live
) {
    return filter_column_impl<double>(
        data, count, rhs, op, nulls, live,
        [](const double* p, size_t n, double v, CmpOp c) noexcept {
            return zephyr::cpp26::simd::filter_chunk_f64(p, n, v, c);
        }
    );
}

int64_t simd_sum_i64(const int64_t* data, size_t count) noexcept {
    if (!data || count == 0) return 0;
    return zephyr::cpp26::simd::reduce_sum_i64(data, count);
}

} // namespace zephyr::sql
