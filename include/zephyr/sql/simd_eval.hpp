#ifndef ZEPHYR_SQL_SIMD_EVAL_HPP
#define ZEPHYR_SQL_SIMD_EVAL_HPP

#include "zephyr/compat/cpp26.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace zephyr::sql {

using CmpOp = zephyr::cpp26::simd::cmp_op;

/// Vectorized columnar filter over contiguous int64_t values in 64-element chunks.
/// Optional `nulls` (1 = NULL, 0 = non-null) and `live` (1 = live row, 0 = deleted)
/// byte arrays of length `count` are respected when non-null.
std::vector<uint32_t> filter_column_i64(
    const int64_t* data,
    size_t count,
    int64_t rhs,
    CmpOp op,
    const uint8_t* nulls = nullptr,
    const uint8_t* live = nullptr
);

/// Vectorized columnar filter over contiguous double values in 64-element chunks.
std::vector<uint32_t> filter_column_f64(
    const double* data,
    size_t count,
    double rhs,
    CmpOp op,
    const uint8_t* nulls = nullptr,
    const uint8_t* live = nullptr
);

/// Vectorized 64-element chunk mask helper for i64.
inline uint64_t filter_chunk_mask_i64(const int64_t* data, size_t count, int64_t rhs, CmpOp op) noexcept {
    return zephyr::cpp26::simd::filter_chunk_i64(data, count, rhs, op);
}

/// Vectorized 64-element chunk mask helper for f64.
inline uint64_t filter_chunk_mask_f64(const double* data, size_t count, double rhs, CmpOp op) noexcept {
    return zephyr::cpp26::simd::filter_chunk_f64(data, count, rhs, op);
}

/// SIMD-accelerated sum reduction for contiguous int64_t column.
int64_t simd_sum_i64(const int64_t* data, size_t count) noexcept;

} // namespace zephyr::sql

#endif // ZEPHYR_SQL_SIMD_EVAL_HPP
