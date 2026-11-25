#ifndef ZEPHYR_STORAGE_TIERED_STORE_HPP
#define ZEPHYR_STORAGE_TIERED_STORE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zephyr::storage {

struct ColdRow {
    int64_t key = 0;
    uint64_t row_id = 0;
    std::array<int64_t, 8> i64_cols{};
    std::array<double, 8> f64_cols{};
};

struct SegmentZoneMap {
    uint64_t segment_id = 0;
    int64_t min_key = 0;
    int64_t max_key = 0;
    uint64_t row_count = 0;
    std::array<int64_t, 8> sum_i64{};
    std::array<double, 8> sum_f64{};
    bool encrypted = false;
    std::string file_path;
};

/// Hot/Cold Tiered Mmap Segment Store.
/// Flushes sorted ColdRow batches into immutable `.zseg` files (plaintext `"ZSEG"`
/// readable via MmapStore, or AEAD-encrypted `"ZSNE"` when `enc_key32 != nullptr`),
/// maintaining per-segment zone maps for O(1) aggregates and segment pruning.
class TieredStore {
public:
    static constexpr uint32_t SEGMENT_MAGIC = 0x5A534547u; // "ZSEG"

    explicit TieredStore(std::string dir = "");

    void set_directory(std::string dir);

    /// Sort `rows` by `key`, compute its SegmentZoneMap, and write an immutable `.zseg` file.
    /// Writes plaintext `"ZSEG"` if `enc_key32 == nullptr`, or encrypted `"ZSNE"` otherwise.
    bool flush_cold_segment(std::string_view table_name,
                            std::vector<ColdRow> rows,
                            const uint8_t* enc_key32 = nullptr);

    /// O(1) per-segment total row count across all cold segments for `table_name`.
    uint64_t cold_total_count(std::string_view table_name) const;

    /// O(1) per-segment total sum of `i64_cols[col]` across all cold segments for `table_name`.
    int64_t cold_total_sum_i64(std::string_view table_name, size_t col = 0) const;

    /// Range sum of `i64_cols[col]` for `key in [lo, hi]`.
    /// Prunes segments via [min_key, max_key], uses full-segment `sum_i64` when
    /// `[min_key, max_key] <= [lo, hi]`, and binary-searches partially overlapping segments.
    int64_t cold_range_sum_i64(std::string_view table_name,
                               int64_t lo,
                               int64_t hi,
                               size_t col = 0) const;

    /// Point lookup by `key` across cold segments, pruning via zone maps and
    /// binary-searching the mmap'd segment (or decrypted buffer if encrypted).
    bool cold_find(std::string_view table_name,
                   int64_t key,
                   ColdRow& out_row,
                   const uint8_t* enc_key32 = nullptr) const;

    size_t segment_count(std::string_view table_name) const;

private:
    mutable std::shared_mutex mu_;
    std::string dir_;
    uint64_t next_segment_id_{1};
    std::unordered_map<std::string, std::vector<SegmentZoneMap>> tables_;
};

} // namespace zephyr::storage

#endif // ZEPHYR_STORAGE_TIERED_STORE_HPP
