#include "zephyr/storage/wal.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
  #include <io.h>
#elif !defined(__EMSCRIPTEN__)
  #include <unistd.h>
#endif

namespace zephyr::storage {

namespace {

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) noexcept {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}

void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

void write_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

uint32_t read_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64_le(const uint8_t* p) noexcept {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    }
    return v;
}

void serialize_record(std::vector<uint8_t>& out, uint64_t lsn, uint8_t op, std::string_view sql) {
    const uint32_t len = static_cast<uint32_t>(sql.size());
    const size_t start = out.size();
    write_u32_le(out, WalManager::MAGIC);
    const size_t crc_start = out.size();
    write_u64_le(out, lsn);
    out.push_back(op);
    write_u32_le(out, len);
    if (len > 0) {
        out.insert(out.end(), sql.begin(), sql.end());
    }
    const uint32_t crc = crc32_update(0, out.data() + crc_start, out.size() - crc_start);
    write_u32_le(out, crc);
    (void)start;
}

} // namespace

// ----------------------------------------------------------------------------
// WalRingBuffer
// ----------------------------------------------------------------------------

bool WalRingBuffer::try_push(uint64_t lsn, uint8_t op, std::string_view sql) {
    const size_t h = head_.load(std::memory_order_relaxed);
    const size_t next = (h + 1) & (CAPACITY - 1);
    if (next == tail_.load(std::memory_order_acquire)) {
        return false; // full
    }
    buffer_[h].lsn = lsn;
    buffer_[h].op = op;
    buffer_[h].sql.assign(sql.data(), sql.size());
    head_.store(next, std::memory_order_release);
    return true;
}

bool WalRingBuffer::try_pop(Slot& out) {
    const size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) {
        return false; // empty
    }
    out = std::move(buffer_[t]);
    tail_.store((t + 1) & (CAPACITY - 1), std::memory_order_release);
    return true;
}

size_t WalRingBuffer::pop_batch(std::vector<Slot>& out, size_t max_batch) {
    out.clear();
    Slot s;
    while (out.size() < max_batch && try_pop(s)) {
        out.push_back(std::move(s));
    }
    return out.size();
}

bool WalRingBuffer::empty() const noexcept {
    return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
}

// ----------------------------------------------------------------------------
// WalManager (WAL Guardian)
// ----------------------------------------------------------------------------

WalManager::~WalManager() {
    close();
}

bool WalManager::open(const std::string& wal_path, uint32_t flush_ms) {
    if (open_.load(std::memory_order_acquire)) {
        return true;
    }
    wal_path_ = wal_path;
    flush_ms_ = (flush_ms == 0) ? 1 : flush_ms;

    std::error_code ec;
    const auto parent = std::filesystem::path(wal_path_).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    // Touch / create file if missing
    {
        std::FILE* fp = std::fopen(wal_path_.c_str(), "ab");
        if (!fp) {
            return false;
        }
        std::fclose(fp);
    }

    // Recover highest LSN from existing records
    uint64_t max_lsn = 0;
    for (const auto& rec : replay()) {
        if (rec.lsn > max_lsn) {
            max_lsn = rec.lsn;
        }
    }
    next_lsn_.store(max_lsn + 1, std::memory_order_release);
    flushed_lsn_.store(max_lsn, std::memory_order_release);

    open_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    cpp26::execution::start_detached(
        cpp26::execution::schedule_task(sched_, [this] { guardian_loop(); })
    );
    return true;
}

void WalManager::close() {
    if (!open_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    running_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        wake_requested_ = true;
    }
    wake_cv_.notify_all();
    sched_.stop();
    drain_and_write();
}

uint64_t WalManager::append(uint8_t op, std::string_view sql, bool sync_wait) {
    if (!open_.load(std::memory_order_acquire)) {
        return 0;
    }

    uint64_t lsn = 0;
    {
        std::lock_guard<std::mutex> lk(push_mu_);
        lsn = next_lsn_.fetch_add(1, std::memory_order_acq_rel);
        while (!ring_.try_push(lsn, op, sql)) {
            {
                std::lock_guard<std::mutex> clk(cv_mu_);
                wake_requested_ = true;
            }
            wake_cv_.notify_one();
            std::this_thread::yield();
        }
    }

    if (sync_wait) {
        {
            std::lock_guard<std::mutex> clk(cv_mu_);
            wake_requested_ = true;
        }
        wake_cv_.notify_one();

        std::unique_lock<std::mutex> lk(cv_mu_);
        flush_cv_.wait(lk, [this, lsn] {
            return flushed_lsn_.load(std::memory_order_acquire) >= lsn ||
                   !running_.load(std::memory_order_acquire);
        });
    }
    return lsn;
}

void WalManager::flush() {
    if (!open_.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t target = next_lsn_.load(std::memory_order_acquire) - 1;
    if (flushed_lsn_.load(std::memory_order_acquire) >= target && ring_.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        wake_requested_ = true;
    }
    wake_cv_.notify_one();

    std::unique_lock<std::mutex> lk(cv_mu_);
    flush_cv_.wait(lk, [this, target] {
        return (flushed_lsn_.load(std::memory_order_acquire) >= target && ring_.empty()) ||
               !running_.load(std::memory_order_acquire);
    });
}

void WalManager::guardian_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(cv_mu_);
            wake_cv_.wait_for(lk, std::chrono::milliseconds(flush_ms_), [this] {
                return !running_.load(std::memory_order_acquire) || wake_requested_;
            });
            wake_requested_ = false;
        }
        drain_and_write();
    }
}

void WalManager::drain_and_write() {
    std::vector<WalRingBuffer::Slot> batch;
    std::vector<uint8_t> encoded;

    while (ring_.pop_batch(batch, 256) > 0) {
        encoded.clear();
        uint64_t max_lsn = flushed_lsn_.load(std::memory_order_relaxed);
        for (const auto& s : batch) {
            serialize_record(encoded, s.lsn, s.op, s.sql);
            if (s.lsn > max_lsn) {
                max_lsn = s.lsn;
            }
        }

        {
            std::lock_guard<std::mutex> flk(file_mu_);
            std::FILE* fp = std::fopen(wal_path_.c_str(), "ab");
            if (fp) {
                if (!encoded.empty()) {
                    std::fwrite(encoded.data(), 1, encoded.size(), fp);
                }
                std::fflush(fp);
#if defined(_WIN32)
                _commit(_fileno(fp));
#elif !defined(__EMSCRIPTEN__)
                fsync(fileno(fp));
#endif
                std::fclose(fp);
            }
        }

        flushed_lsn_.store(max_lsn, std::memory_order_release);
        flush_cv_.notify_all();
    }
}

std::vector<WalRecord> WalManager::replay() const {
    std::vector<WalRecord> records;
    std::lock_guard<std::mutex> flk(const_cast<std::mutex&>(file_mu_));

    std::FILE* fp = std::fopen(wal_path_.c_str(), "rb");
    if (!fp) {
        return records;
    }

    std::fseek(fp, 0, SEEK_END);
    const long fsize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
        std::fclose(fp);
        return records;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    const size_t read_bytes = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    buf.resize(read_bytes);

    size_t pos = 0;
    // Header: 4 (magic) + 8 (lsn) + 1 (op) + 4 (len) = 17 bytes, + 4 (crc) = 21 bytes min
    while (pos + 21 <= buf.size()) {
        const uint32_t magic = read_u32_le(buf.data() + pos);
        if (magic != MAGIC) {
            break;
        }
        const size_t crc_start = pos + 4;
        const uint64_t lsn = read_u64_le(buf.data() + pos + 4);
        const uint8_t op = buf[pos + 12];
        const uint32_t len = read_u32_le(buf.data() + pos + 13);

        if (pos + 21 + len > buf.size()) {
            break; // partial tail record
        }

        const uint32_t expected_crc = read_u32_le(buf.data() + pos + 17 + len);
        const uint32_t actual_crc = crc32_update(0, buf.data() + crc_start, 13 + len);
        if (expected_crc != actual_crc) {
            break; // corrupt tail record
        }

        WalRecord rec;
        rec.lsn = lsn;
        rec.op = op;
        if (len > 0) {
            rec.sql.assign(reinterpret_cast<const char*>(buf.data() + pos + 17), len);
        }
        records.push_back(std::move(rec));
        pos += 21 + len;
    }

    return records;
}

void WalManager::truncate(uint64_t up_to_lsn) {
    flush();
    std::vector<WalRecord> keep;
    if (up_to_lsn < UINT64_MAX) {
        for (auto& rec : replay()) {
            if (rec.lsn > up_to_lsn) {
                keep.push_back(std::move(rec));
            }
        }
    }

    std::vector<uint8_t> encoded;
    for (const auto& rec : keep) {
        serialize_record(encoded, rec.lsn, rec.op, rec.sql);
    }

    std::lock_guard<std::mutex> flk(file_mu_);
    std::FILE* fp = std::fopen(wal_path_.c_str(), "wb");
    if (fp) {
        if (!encoded.empty()) {
            std::fwrite(encoded.data(), 1, encoded.size(), fp);
        }
        std::fflush(fp);
        std::fclose(fp);
    }
}

} // namespace zephyr::storage
