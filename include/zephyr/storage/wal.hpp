#ifndef ZEPHYR_STORAGE_WAL_HPP
#define ZEPHYR_STORAGE_WAL_HPP

#include "zephyr/compat/cpp26.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace zephyr::storage {

struct WalRecord {
    uint64_t lsn = 0;
    uint8_t op = 0;
    std::string sql;
};

/// Single-consumer lock-free ring buffer for WAL entries.
/// Producers are synchronized via a lightweight spin/mutex on append;
/// the WAL Guardian consumer drains batches without blocking producers.
class WalRingBuffer {
public:
    static constexpr size_t CAPACITY = 4096; // must be power of two

    struct Slot {
        uint64_t lsn = 0;
        uint8_t op = 0;
        std::string sql;
    };

    WalRingBuffer() = default;

    bool try_push(uint64_t lsn, uint8_t op, std::string_view sql);
    bool try_pop(Slot& out);
    size_t pop_batch(std::vector<Slot>& out, size_t max_batch = 256);
    bool empty() const noexcept;

private:
    std::array<Slot, CAPACITY> buffer_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

/// WAL Guardian (Disk I/O Agent, see AGENTS.md).
/// Batches committed transactions from WalRingBuffer and flushes them
/// asynchronously to the physical WAL file on a background execution context.
class WalManager {
public:
    static constexpr uint32_t MAGIC = 0x5A57414Cu; // "ZWAL"

    WalManager() = default;
    ~WalManager();

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    bool open(const std::string& wal_path, uint32_t flush_ms = 2);
    void close();

    /// Enqueue a WAL record. If `sync_wait` is true, blocks until flushed to disk.
    /// Returns the assigned LSN (or 0 if WAL is not open).
    uint64_t append(uint8_t op, std::string_view sql, bool sync_wait = false);

    /// Force-flush all queued records to disk and wait for completion.
    void flush();

    /// Replay all valid, CRC-verified records from the WAL file on disk.
    std::vector<WalRecord> replay() const;

    /// Truncate WAL records up to `up_to_lsn` (UINT64_MAX truncates the entire file).
    void truncate(uint64_t up_to_lsn = UINT64_MAX);

    bool is_open() const noexcept { return open_.load(std::memory_order_acquire); }
    uint64_t last_lsn() const noexcept { return next_lsn_.load(std::memory_order_acquire) - 1; }
    uint64_t flushed_lsn() const noexcept { return flushed_lsn_.load(std::memory_order_acquire); }
    const std::string& path() const noexcept { return wal_path_; }

private:
    void guardian_loop();
    void drain_and_write();

    std::string wal_path_;
    uint32_t flush_ms_{2};

    WalRingBuffer ring_;
    std::mutex push_mu_;
    std::mutex file_mu_;

    std::mutex cv_mu_;
    std::condition_variable wake_cv_;
    std::condition_variable flush_cv_;
    bool wake_requested_{false};

    cpp26::execution::thread_pool_scheduler sched_{1};
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_lsn_{1};
    std::atomic<uint64_t> flushed_lsn_{0};
};

} // namespace zephyr::storage

#endif // ZEPHYR_STORAGE_WAL_HPP
