#include "zephyr/storage/tiered_store.hpp"
#include "zephyr/storage/mmap_store.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <mutex>

namespace zephyr::storage {

namespace {

constexpr uint32_t kSegmentVersion = 1u;
constexpr size_t kHeaderBytes = 40;
constexpr size_t kRowBytes = 144; // key(8) + row_id(8) + i64_cols(64) + f64_cols(64)

inline void store_le32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void store_le64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFu);
    }
}

inline uint32_t load_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t load_le64(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

inline int64_t load_le_i64(const uint8_t* p) noexcept {
    return static_cast<int64_t>(load_le64(p));
}

inline void store_le_i64(uint8_t* p, int64_t v) noexcept {
    store_le64(p, static_cast<uint64_t>(v));
}

void encode_row(uint8_t* dst, const ColdRow& row) noexcept {
    store_le_i64(dst, row.key);
    store_le64(dst + 8, row.row_id);
    for (size_t i = 0; i < 8; ++i) {
        store_le_i64(dst + 16 + i * 8, row.i64_cols[i]);
    }
    for (size_t i = 0; i < 8; ++i) {
        uint64_t bits = 0;
        std::memcpy(&bits, &row.f64_cols[i], sizeof(bits));
        store_le64(dst + 80 + i * 8, bits);
    }
}

void decode_row(const uint8_t* src, ColdRow& out) noexcept {
    out.key = load_le_i64(src);
    out.row_id = load_le64(src + 8);
    for (size_t i = 0; i < 8; ++i) {
        out.i64_cols[i] = load_le_i64(src + 16 + i * 8);
    }
    for (size_t i = 0; i < 8; ++i) {
        const uint64_t bits = load_le64(src + 80 + i * 8);
        std::memcpy(&out.f64_cols[i], &bits, sizeof(bits));
    }
}

bool validate_segment_buffer(const uint8_t* data, size_t size, uint64_t& out_row_count) noexcept {
    if (!data || size < kHeaderBytes) {
        return false;
    }
    const uint32_t magic = load_le32(data);
    const uint32_t version = load_le32(data + 4);
    if (magic != TieredStore::SEGMENT_MAGIC || version != kSegmentVersion) {
        return false;
    }
    const uint64_t count = load_le64(data + 16);
    if (count > (size - kHeaderBytes) / kRowBytes) {
        return false;
    }
    if (size != kHeaderBytes + static_cast<size_t>(count) * kRowBytes) {
        return false;
    }
    out_row_count = count;
    return true;
}

/// Binary-search for the first row index whose key >= `target_key`.
uint64_t lower_bound_key(const uint8_t* rows_base, uint64_t row_count, int64_t target_key) noexcept {
    uint64_t lo = 0;
    uint64_t hi = row_count;
    while (lo < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        const int64_t mid_key = load_le_i64(rows_base + static_cast<size_t>(mid) * kRowBytes);
        if (mid_key < target_key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::string sanitize_table_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) {
        out = "table";
    }
    return out;
}

} // namespace

TieredStore::TieredStore(std::string dir) : dir_(std::move(dir)) {}

void TieredStore::set_directory(std::string dir) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    dir_ = std::move(dir);
}

bool TieredStore::flush_cold_segment(std::string_view table_name,
                                     std::vector<ColdRow> rows,
                                     const uint8_t* enc_key32) {
    if (table_name.empty() || rows.empty()) {
        return false;
    }

    std::stable_sort(rows.begin(), rows.end(), [](const ColdRow& a, const ColdRow& b) {
        return a.key < b.key;
    });

    uint64_t seg_id = 0;
    std::string file_path;
    {
        std::unique_lock<std::shared_mutex> lk(mu_);
        seg_id = next_segment_id_++;
        const std::string fname =
            sanitize_table_name(table_name) + "_seg_" + std::to_string(seg_id) + ".zseg";
        if (dir_.empty()) {
            file_path = fname;
        } else {
            file_path = (std::filesystem::path(dir_) / fname).string();
        }
    }

    SegmentZoneMap zm;
    zm.segment_id = seg_id;
    zm.min_key = rows.front().key;
    zm.max_key = rows.back().key;
    zm.row_count = static_cast<uint64_t>(rows.size());
    zm.encrypted = (enc_key32 != nullptr);
    zm.file_path = file_path;

    std::vector<uint8_t> blob(kHeaderBytes + rows.size() * kRowBytes);
    store_le32(blob.data(), SEGMENT_MAGIC);
    store_le32(blob.data() + 4, kSegmentVersion);
    store_le64(blob.data() + 8, zm.segment_id);
    store_le64(blob.data() + 16, zm.row_count);
    store_le_i64(blob.data() + 24, zm.min_key);
    store_le_i64(blob.data() + 32, zm.max_key);

    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        for (size_t c = 0; c < 8; ++c) {
            zm.sum_i64[c] += r.i64_cols[c];
            zm.sum_f64[c] += r.f64_cols[c];
        }
        encode_row(blob.data() + kHeaderBytes + i * kRowBytes, r);
    }

    MmapStore writer;
    const bool ok = (enc_key32 != nullptr)
        ? writer.write_snapshot_encrypted(file_path, blob, enc_key32)
        : writer.write_snapshot(file_path, blob);
    if (!ok) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lk(mu_);
    tables_[std::string(table_name)].push_back(std::move(zm));
    return true;
}

uint64_t TieredStore::cold_total_count(std::string_view table_name) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    const auto it = tables_.find(std::string(table_name));
    if (it == tables_.end()) {
        return 0;
    }
    uint64_t total = 0;
    for (const auto& zm : it->second) {
        total += zm.row_count;
    }
    return total;
}

int64_t TieredStore::cold_total_sum_i64(std::string_view table_name, size_t col) const {
    if (col >= 8) {
        return 0;
    }
    std::shared_lock<std::shared_mutex> lk(mu_);
    const auto it = tables_.find(std::string(table_name));
    if (it == tables_.end()) {
        return 0;
    }
    int64_t total = 0;
    for (const auto& zm : it->second) {
        total += zm.sum_i64[col];
    }
    return total;
}

int64_t TieredStore::cold_range_sum_i64(std::string_view table_name,
                                          int64_t lo,
                                          int64_t hi,
                                          size_t col) const {
    if (lo > hi || col >= 8) {
        return 0;
    }

    std::vector<SegmentZoneMap> segments;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        const auto it = tables_.find(std::string(table_name));
        if (it == tables_.end()) {
            return 0;
        }
        segments = it->second;
    }

    int64_t total = 0;
    for (const auto& zm : segments) {
        if (zm.max_key < lo || zm.min_key > hi) {
            continue; // pruned
        }
        if (zm.min_key >= lo && zm.max_key <= hi) {
            total += zm.sum_i64[col];
            continue;
        }
        if (zm.encrypted) {
            continue;
        }

        MmapStore reader;
        if (!reader.open_mmap(zm.file_path)) {
            continue;
        }
        uint64_t row_count = 0;
        if (!validate_segment_buffer(reader.data(), reader.size(), row_count)) {
            continue;
        }
        const uint8_t* rows_base = reader.data() + kHeaderBytes;
        for (uint64_t idx = lower_bound_key(rows_base, row_count, lo); idx < row_count; ++idx) {
            const uint8_t* row_ptr = rows_base + static_cast<size_t>(idx) * kRowBytes;
            const int64_t k = load_le_i64(row_ptr);
            if (k > hi) {
                break;
            }
            total += load_le_i64(row_ptr + 16 + col * 8);
        }
    }
    return total;
}

bool TieredStore::cold_find(std::string_view table_name,
                            int64_t key,
                            ColdRow& out_row,
                            const uint8_t* enc_key32) const {
    std::vector<SegmentZoneMap> segments;
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        const auto it = tables_.find(std::string(table_name));
        if (it == tables_.end()) {
            return false;
        }
        segments = it->second;
    }

    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        const auto& zm = *it;
        if (key < zm.min_key || key > zm.max_key) {
            continue; // pruned
        }

        const uint8_t* buf_data = nullptr;
        size_t buf_size = 0;
        MmapStore mmap_reader;
        std::vector<uint8_t> decrypted_buf;

        if (zm.encrypted) {
            if (enc_key32 == nullptr) {
                continue;
            }
            if (!mmap_reader.read_snapshot_encrypted(zm.file_path, enc_key32, decrypted_buf)) {
                continue;
            }
            buf_data = decrypted_buf.data();
            buf_size = decrypted_buf.size();
        } else {
            if (!mmap_reader.open_mmap(zm.file_path)) {
                continue;
            }
            buf_data = mmap_reader.data();
            buf_size = mmap_reader.size();
        }

        uint64_t row_count = 0;
        if (!validate_segment_buffer(buf_data, buf_size, row_count)) {
            continue;
        }

        const uint8_t* rows_base = buf_data + kHeaderBytes;
        const uint64_t idx = lower_bound_key(rows_base, row_count, key);
        if (idx < row_count) {
            const uint8_t* row_ptr = rows_base + static_cast<size_t>(idx) * kRowBytes;
            if (load_le_i64(row_ptr) == key) {
                decode_row(row_ptr, out_row);
                return true;
            }
        }
    }
    return false;
}

size_t TieredStore::segment_count(std::string_view table_name) const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    const auto it = tables_.find(std::string(table_name));
    return (it == tables_.end()) ? 0 : it->second.size();
}

} // namespace zephyr::storage
