#include "zephyr/sql/simd_eval.hpp"

#include <algorithm>
#include <bit>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace zephyr::sql {

namespace {

[[maybe_unused]] inline uint32_t ctz32(uint32_t x) noexcept {
#if defined(_MSC_VER) && defined(__AVX2__)
    return _tzcnt_u32(x);
#elif defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward(&idx, x);
    return static_cast<uint32_t>(idx);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctz(x));
#else
    return static_cast<uint32_t>(std::countr_zero(x));
#endif
}

[[maybe_unused]] inline uint32_t ctz64(uint64_t x) noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64))
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return static_cast<uint32_t>(idx);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_ctzll(x));
#else
    return static_cast<uint32_t>(std::countr_zero(x));
#endif
}

[[maybe_unused]] inline uint64_t popcnt64(uint64_t x) noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)) && defined(__AVX2__)
    return __popcnt64(x);
#else
    return static_cast<uint64_t>(std::popcount(x));
#endif
}

template <CmpOp Op, typename T>
inline bool eval_cmp_t(T a, T b) noexcept {
    if constexpr (Op == CmpOp::eq) return a == b;
    else if constexpr (Op == CmpOp::ne) return a != b;
    else if constexpr (Op == CmpOp::lt) return a < b;
    else if constexpr (Op == CmpOp::le) return a <= b;
    else if constexpr (Op == CmpOp::gt) return a > b;
    else /* CmpOp::ge */ return a >= b;
}

#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)

template <CmpOp Op>
consteval bool avx2_i64_invert() noexcept {
    return Op == CmpOp::ne || Op == CmpOp::ge || Op == CmpOp::le;
}

template <CmpOp Op>
inline __m256i avx2_cmp_raw_i64(__m256i v, __m256i vrhs) noexcept {
    if constexpr (Op == CmpOp::eq || Op == CmpOp::ne) {
        return _mm256_cmpeq_epi64(v, vrhs);
    } else if constexpr (Op == CmpOp::gt || Op == CmpOp::le) {
        return _mm256_cmpgt_epi64(v, vrhs);
    } else { // CmpOp::lt || CmpOp::ge
        return _mm256_cmpgt_epi64(vrhs, v);
    }
}

template <CmpOp Op>
inline uint32_t avx2_mask16_i64(const int64_t* p, __m256i vrhs) noexcept {
    const __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 4));
    const __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 8));
    const __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 12));

    const uint32_t m0 = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(avx2_cmp_raw_i64<Op>(v0, vrhs))));
    const uint32_t m1 = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(avx2_cmp_raw_i64<Op>(v1, vrhs))));
    const uint32_t m2 = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(avx2_cmp_raw_i64<Op>(v2, vrhs))));
    const uint32_t m3 = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(avx2_cmp_raw_i64<Op>(v3, vrhs))));

    uint32_t mask = m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
    if constexpr (avx2_i64_invert<Op>()) {
        mask ^= 0xFFFFu;
    }
    return mask;
}

template <CmpOp Op>
inline __m256d avx2_cmp_f64(__m256d v, __m256d vrhs) noexcept {
    if constexpr (Op == CmpOp::eq)      return _mm256_cmp_pd(v, vrhs, _CMP_EQ_OQ);
    else if constexpr (Op == CmpOp::ne) return _mm256_cmp_pd(v, vrhs, _CMP_NEQ_UQ);
    else if constexpr (Op == CmpOp::lt) return _mm256_cmp_pd(v, vrhs, _CMP_LT_OQ);
    else if constexpr (Op == CmpOp::le) return _mm256_cmp_pd(v, vrhs, _CMP_LE_OQ);
    else if constexpr (Op == CmpOp::gt) return _mm256_cmp_pd(v, vrhs, _CMP_GT_OQ);
    else /* CmpOp::ge */                return _mm256_cmp_pd(v, vrhs, _CMP_GE_OQ);
}

template <CmpOp Op>
inline uint32_t avx2_mask16_f64(const double* p, __m256d vrhs) noexcept {
    const __m256d v0 = _mm256_loadu_pd(p);
    const __m256d v1 = _mm256_loadu_pd(p + 4);
    const __m256d v2 = _mm256_loadu_pd(p + 8);
    const __m256d v3 = _mm256_loadu_pd(p + 12);

    const uint32_t m0 = static_cast<uint32_t>(_mm256_movemask_pd(avx2_cmp_f64<Op>(v0, vrhs)));
    const uint32_t m1 = static_cast<uint32_t>(_mm256_movemask_pd(avx2_cmp_f64<Op>(v1, vrhs)));
    const uint32_t m2 = static_cast<uint32_t>(_mm256_movemask_pd(avx2_cmp_f64<Op>(v2, vrhs)));
    const uint32_t m3 = static_cast<uint32_t>(_mm256_movemask_pd(avx2_cmp_f64<Op>(v3, vrhs)));

    return m0 | (m1 << 4) | (m2 << 8) | (m3 << 12);
}

inline uint32_t avx2_valid_mask16(
    const uint8_t* nulls,
    const uint8_t* live,
    size_t offset,
    __m128i vzero
) noexcept {
    uint32_t valid = 0xFFFFu;
    if (nulls) {
        const __m128i vn = _mm_loadu_si128(reinterpret_cast<const __m128i*>(nulls + offset));
        valid &= static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(vn, vzero)));
    }
    if (live) {
        const __m128i vl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(live + offset));
        valid &= (static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(vl, vzero))) ^ 0xFFFFu);
    }
    return valid;
}

#elif defined(ZEPHYR_HAS_WASM_SIMD)

template <CmpOp Op>
inline v128_t wasm_cmp_i64(v128_t v, v128_t vrhs) noexcept {
    if constexpr (Op == CmpOp::eq)      return wasm_i64x2_eq(v, vrhs);
    else if constexpr (Op == CmpOp::ne) return wasm_i64x2_ne(v, vrhs);
    else if constexpr (Op == CmpOp::lt) return wasm_i64x2_lt(v, vrhs);
    else if constexpr (Op == CmpOp::le) return wasm_i64x2_le(v, vrhs);
    else if constexpr (Op == CmpOp::gt) return wasm_i64x2_gt(v, vrhs);
    else /* CmpOp::ge */                return wasm_i64x2_ge(v, vrhs);
}

template <CmpOp Op>
inline v128_t wasm_cmp_f64(v128_t v, v128_t vrhs) noexcept {
    if constexpr (Op == CmpOp::eq)      return wasm_f64x2_eq(v, vrhs);
    else if constexpr (Op == CmpOp::ne) return wasm_f64x2_ne(v, vrhs);
    else if constexpr (Op == CmpOp::lt) return wasm_f64x2_lt(v, vrhs);
    else if constexpr (Op == CmpOp::le) return wasm_f64x2_le(v, vrhs);
    else if constexpr (Op == CmpOp::gt) return wasm_f64x2_gt(v, vrhs);
    else /* CmpOp::ge */                return wasm_f64x2_ge(v, vrhs);
}

template <CmpOp Op>
inline uint64_t wasm_chunk_mask_i64(const int64_t* data, size_t chunk_len, v128_t vrhs, int64_t rhs) noexcept {
    uint64_t mask = 0;
    size_t i = 0;
    for (; i + 2 <= chunk_len; i += 2) {
        const v128_t v = wasm_v128_load(data + i);
        const uint32_t m = static_cast<uint32_t>(wasm_i64x2_bitmask(wasm_cmp_i64<Op>(v, vrhs)));
        mask |= (static_cast<uint64_t>(m) << i);
    }
    for (; i < chunk_len; ++i) {
        if (eval_cmp_t<Op>(data[i], rhs)) {
            mask |= (1ULL << i);
        }
    }
    return mask;
}

template <CmpOp Op>
inline uint64_t wasm_chunk_mask_f64(const double* data, size_t chunk_len, v128_t vrhs, double rhs) noexcept {
    uint64_t mask = 0;
    size_t i = 0;
    for (; i + 2 <= chunk_len; i += 2) {
        const v128_t v = wasm_v128_load(data + i);
        const uint32_t m = static_cast<uint32_t>(wasm_i64x2_bitmask(wasm_cmp_f64<Op>(v, vrhs)));
        mask |= (static_cast<uint64_t>(m) << i);
    }
    for (; i < chunk_len; ++i) {
        if (eval_cmp_t<Op>(data[i], rhs)) {
            mask |= (1ULL << i);
        }
    }
    return mask;
}

#endif

template <CmpOp Op>
std::vector<uint32_t> filter_column_i64_t(
    const int64_t* data,
    size_t count,
    int64_t rhs,
    const uint8_t* nulls,
    const uint8_t* live
) {
    std::vector<uint32_t> out;
    if (!data || count == 0) return out;

#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)
    const __m256i vrhs = _mm256_set1_epi64x(rhs);
    if (nulls == nullptr && live == nullptr) {
        out.resize(count);
        uint32_t* dst = out.data();
        size_t i = 0;
        for (; i + 16 <= count; i += 16) {
            uint32_t mask = avx2_mask16_i64<Op>(data + i, vrhs);
            const uint32_t base = static_cast<uint32_t>(i);
            while (mask != 0) {
                *dst++ = base + ctz32(mask);
                mask &= (mask - 1u);
            }
        }
        for (; i + 4 <= count; i += 4) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(avx2_cmp_raw_i64<Op>(v, vrhs))));
            if constexpr (avx2_i64_invert<Op>()) {
                mask ^= 0xFu;
            }
            const uint32_t base = static_cast<uint32_t>(i);
            while (mask != 0) {
                *dst++ = base + ctz32(mask);
                mask &= (mask - 1u);
            }
        }
        for (; i < count; ++i) {
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
        out.resize(static_cast<size_t>(dst - out.data()));
        return out;
    }

    out.resize(count);
    uint32_t* dst = out.data();
    const __m128i vzero = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        uint32_t mask = avx2_mask16_i64<Op>(data + i, vrhs) & avx2_valid_mask16(nulls, live, i, vzero);
        const uint32_t base = static_cast<uint32_t>(i);
        while (mask != 0) {
            *dst++ = base + ctz32(mask);
            mask &= (mask - 1u);
        }
    }
    for (; i < count; ++i) {
        if (nulls && nulls[i] != 0) continue;
        if (live && live[i] == 0) continue;
        if (eval_cmp_t<Op>(data[i], rhs)) {
            *dst++ = static_cast<uint32_t>(i);
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;

#elif defined(ZEPHYR_HAS_WASM_SIMD)
    const v128_t vrhs = wasm_i64x2_splat(rhs);
    out.resize(count);
    uint32_t* dst = out.data();
    for (size_t base = 0; base < count; base += 64) {
        const size_t chunk_len = std::min<size_t>(64, count - base);
        uint64_t mask = wasm_chunk_mask_i64<Op>(data + base, chunk_len, vrhs, rhs);
        while (mask != 0) {
            const uint32_t bit = ctz64(mask);
            mask &= (mask - 1ULL);
            const size_t row_idx = base + bit;
            if (nulls && nulls[row_idx] != 0) continue;
            if (live && live[row_idx] == 0) continue;
            *dst++ = static_cast<uint32_t>(row_idx);
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;

#else
    out.resize(count);
    uint32_t* dst = out.data();
    if (nulls == nullptr && live == nullptr) {
        for (size_t i = 0; i < count; ++i) {
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (nulls && nulls[i] != 0) continue;
            if (live && live[i] == 0) continue;
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;
#endif
}

template <CmpOp Op>
std::vector<uint32_t> filter_column_f64_t(
    const double* data,
    size_t count,
    double rhs,
    const uint8_t* nulls,
    const uint8_t* live
) {
    std::vector<uint32_t> out;
    if (!data || count == 0) return out;

#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)
    const __m256d vrhs = _mm256_set1_pd(rhs);
    if (nulls == nullptr && live == nullptr) {
        out.resize(count);
        uint32_t* dst = out.data();
        size_t i = 0;
        for (; i + 16 <= count; i += 16) {
            uint32_t mask = avx2_mask16_f64<Op>(data + i, vrhs);
            const uint32_t base = static_cast<uint32_t>(i);
            while (mask != 0) {
                *dst++ = base + ctz32(mask);
                mask &= (mask - 1u);
            }
        }
        for (; i + 4 <= count; i += 4) {
            const __m256d v = _mm256_loadu_pd(data + i);
            uint32_t mask = static_cast<uint32_t>(_mm256_movemask_pd(avx2_cmp_f64<Op>(v, vrhs)));
            const uint32_t base = static_cast<uint32_t>(i);
            while (mask != 0) {
                *dst++ = base + ctz32(mask);
                mask &= (mask - 1u);
            }
        }
        for (; i < count; ++i) {
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
        out.resize(static_cast<size_t>(dst - out.data()));
        return out;
    }

    out.resize(count);
    uint32_t* dst = out.data();
    const __m128i vzero = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        uint32_t mask = avx2_mask16_f64<Op>(data + i, vrhs) & avx2_valid_mask16(nulls, live, i, vzero);
        const uint32_t base = static_cast<uint32_t>(i);
        while (mask != 0) {
            *dst++ = base + ctz32(mask);
            mask &= (mask - 1u);
        }
    }
    for (; i < count; ++i) {
        if (nulls && nulls[i] != 0) continue;
        if (live && live[i] == 0) continue;
        if (eval_cmp_t<Op>(data[i], rhs)) {
            *dst++ = static_cast<uint32_t>(i);
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;

#elif defined(ZEPHYR_HAS_WASM_SIMD)
    const v128_t vrhs = wasm_f64x2_splat(rhs);
    out.resize(count);
    uint32_t* dst = out.data();
    for (size_t base = 0; base < count; base += 64) {
        const size_t chunk_len = std::min<size_t>(64, count - base);
        uint64_t mask = wasm_chunk_mask_f64<Op>(data + base, chunk_len, vrhs, rhs);
        while (mask != 0) {
            const uint32_t bit = ctz64(mask);
            mask &= (mask - 1ULL);
            const size_t row_idx = base + bit;
            if (nulls && nulls[row_idx] != 0) continue;
            if (live && live[row_idx] == 0) continue;
            *dst++ = static_cast<uint32_t>(row_idx);
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;

#else
    out.resize(count);
    uint32_t* dst = out.data();
    if (nulls == nullptr && live == nullptr) {
        for (size_t i = 0; i < count; ++i) {
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (nulls && nulls[i] != 0) continue;
            if (live && live[i] == 0) continue;
            if (eval_cmp_t<Op>(data[i], rhs)) {
                *dst++ = static_cast<uint32_t>(i);
            }
        }
    }
    out.resize(static_cast<size_t>(dst - out.data()));
    return out;
#endif
}

template <CmpOp Op>
uint64_t filter_count_i64_t(
    const int64_t* data,
    size_t count,
    int64_t rhs,
    const uint8_t* nulls,
    const uint8_t* live
) noexcept {
    if (!data || count == 0) return 0;
    uint64_t total = 0;

#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)
    const __m256i vrhs = _mm256_set1_epi64x(rhs);
    size_t i = 0;
    if (nulls == nullptr && live == nullptr) {
        for (; i + 64 <= count; i += 64) {
            const uint64_t w0 = avx2_mask16_i64<Op>(data + i, vrhs);
            const uint64_t w1 = avx2_mask16_i64<Op>(data + i + 16, vrhs);
            const uint64_t w2 = avx2_mask16_i64<Op>(data + i + 32, vrhs);
            const uint64_t w3 = avx2_mask16_i64<Op>(data + i + 48, vrhs);
            const uint64_t mask64 = w0 | (w1 << 16) | (w2 << 32) | (w3 << 48);
            total += popcnt64(mask64);
        }
        for (; i + 16 <= count; i += 16) {
            total += popcnt64(avx2_mask16_i64<Op>(data + i, vrhs));
        }
        for (; i < count; ++i) {
            total += static_cast<uint64_t>(eval_cmp_t<Op>(data[i], rhs));
        }
        return total;
    }

    const __m128i vzero = _mm_setzero_si128();
    for (; i + 64 <= count; i += 64) {
        const uint64_t w0 = avx2_mask16_i64<Op>(data + i, vrhs) & avx2_valid_mask16(nulls, live, i, vzero);
        const uint64_t w1 = avx2_mask16_i64<Op>(data + i + 16, vrhs) & avx2_valid_mask16(nulls, live, i + 16, vzero);
        const uint64_t w2 = avx2_mask16_i64<Op>(data + i + 32, vrhs) & avx2_valid_mask16(nulls, live, i + 32, vzero);
        const uint64_t w3 = avx2_mask16_i64<Op>(data + i + 48, vrhs) & avx2_valid_mask16(nulls, live, i + 48, vzero);
        const uint64_t mask64 = w0 | (w1 << 16) | (w2 << 32) | (w3 << 48);
        total += popcnt64(mask64);
    }
    for (; i + 16 <= count; i += 16) {
        const uint64_t w = avx2_mask16_i64<Op>(data + i, vrhs) & avx2_valid_mask16(nulls, live, i, vzero);
        total += popcnt64(w);
    }
    for (; i < count; ++i) {
        if (nulls && nulls[i] != 0) continue;
        if (live && live[i] == 0) continue;
        total += static_cast<uint64_t>(eval_cmp_t<Op>(data[i], rhs));
    }
    return total;

#elif defined(ZEPHYR_HAS_WASM_SIMD)
    const v128_t vrhs = wasm_i64x2_splat(rhs);
    for (size_t base = 0; base < count; base += 64) {
        const size_t chunk_len = std::min<size_t>(64, count - base);
        uint64_t mask = wasm_chunk_mask_i64<Op>(data + base, chunk_len, vrhs, rhs);
        if (nulls || live) {
            uint64_t valid = 0;
            for (size_t k = 0; k < chunk_len; ++k) {
                const bool ok = (!nulls || nulls[base + k] == 0) && (!live || live[base + k] != 0);
                valid |= (static_cast<uint64_t>(ok) << k);
            }
            mask &= valid;
        }
        total += popcnt64(mask);
    }
    return total;

#else
    if (nulls == nullptr && live == nullptr) {
        for (size_t i = 0; i < count; ++i) {
            total += static_cast<uint64_t>(eval_cmp_t<Op>(data[i], rhs));
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (nulls && nulls[i] != 0) continue;
            if (live && live[i] == 0) continue;
            total += static_cast<uint64_t>(eval_cmp_t<Op>(data[i], rhs));
        }
    }
    return total;
#endif
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
    switch (op) {
        case CmpOp::eq: return filter_column_i64_t<CmpOp::eq>(data, count, rhs, nulls, live);
        case CmpOp::ne: return filter_column_i64_t<CmpOp::ne>(data, count, rhs, nulls, live);
        case CmpOp::lt: return filter_column_i64_t<CmpOp::lt>(data, count, rhs, nulls, live);
        case CmpOp::le: return filter_column_i64_t<CmpOp::le>(data, count, rhs, nulls, live);
        case CmpOp::gt: return filter_column_i64_t<CmpOp::gt>(data, count, rhs, nulls, live);
        case CmpOp::ge: return filter_column_i64_t<CmpOp::ge>(data, count, rhs, nulls, live);
    }
    return {};
}

std::vector<uint32_t> filter_column_f64(
    const double* data,
    size_t count,
    double rhs,
    CmpOp op,
    const uint8_t* nulls,
    const uint8_t* live
) {
    switch (op) {
        case CmpOp::eq: return filter_column_f64_t<CmpOp::eq>(data, count, rhs, nulls, live);
        case CmpOp::ne: return filter_column_f64_t<CmpOp::ne>(data, count, rhs, nulls, live);
        case CmpOp::lt: return filter_column_f64_t<CmpOp::lt>(data, count, rhs, nulls, live);
        case CmpOp::le: return filter_column_f64_t<CmpOp::le>(data, count, rhs, nulls, live);
        case CmpOp::gt: return filter_column_f64_t<CmpOp::gt>(data, count, rhs, nulls, live);
        case CmpOp::ge: return filter_column_f64_t<CmpOp::ge>(data, count, rhs, nulls, live);
    }
    return {};
}

uint64_t filter_count_i64(
    const int64_t* data,
    size_t count,
    int64_t rhs,
    CmpOp op,
    const uint8_t* nulls,
    const uint8_t* live
) noexcept {
    switch (op) {
        case CmpOp::eq: return filter_count_i64_t<CmpOp::eq>(data, count, rhs, nulls, live);
        case CmpOp::ne: return filter_count_i64_t<CmpOp::ne>(data, count, rhs, nulls, live);
        case CmpOp::lt: return filter_count_i64_t<CmpOp::lt>(data, count, rhs, nulls, live);
        case CmpOp::le: return filter_count_i64_t<CmpOp::le>(data, count, rhs, nulls, live);
        case CmpOp::gt: return filter_count_i64_t<CmpOp::gt>(data, count, rhs, nulls, live);
        case CmpOp::ge: return filter_count_i64_t<CmpOp::ge>(data, count, rhs, nulls, live);
    }
    return 0;
}

int64_t simd_sum_i64(const int64_t* data, size_t count) noexcept {
    if (!data || count == 0) return 0;
    return zephyr::cpp26::simd::reduce_sum_i64(data, count);
}

} // namespace zephyr::sql
