#ifndef ZEPHYR_STORAGE_MMAP_STORE_HPP
#define ZEPHYR_STORAGE_MMAP_STORE_HPP

#include "zephyr/compat/cpp26.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace zephyr::storage {

/// Cross-platform memory-mapped snapshot reader and atomic snapshot writer.
/// Uses CreateFileMappingA/MapViewOfFile on Windows, mmap on POSIX, and
/// std::vector<uint8_t> fallback on Emscripten.
class MmapStore {
public:
    static constexpr uint32_t SNAP_ENC_MAGIC = 0x5A534E45u; // "ZSNE"

    MmapStore() = default;
    ~MmapStore();

    MmapStore(const MmapStore&) = delete;
    MmapStore& operator=(const MmapStore&) = delete;

    MmapStore(MmapStore&& other) noexcept;
    MmapStore& operator=(MmapStore&& other) noexcept;

    /// Atomically write a snapshot blob via temp file + replace.
    bool write_snapshot(const std::string& path, const std::vector<uint8_t>& blob);

    /// Atomically write an AEAD-encrypted snapshot via temp file + OS flush + replace.
    /// Format: [magic:4][nonce:12][len:8][ct:len][tag:16]
    bool write_snapshot_encrypted(const std::string& path,
                                  const std::vector<uint8_t>& blob,
                                  const uint8_t key32[32]);

    /// Read and authenticate an AEAD-encrypted snapshot file into `out_blob`.
    /// Sets `*auth_failed = true` (if non-null) on magic/length mismatch or AEAD failure.
    bool read_snapshot_encrypted(const std::string& path,
                                 const uint8_t key32[32],
                                 std::vector<uint8_t>& out_blob,
                                 bool* auth_failed = nullptr);

    /// Memory-map an existing snapshot file read-only.
    bool open_mmap(const std::string& path);

    /// Unmap and close any currently mapped file.
    void close_mmap();

    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool is_open() const noexcept { return data_ != nullptr || size_ > 0; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;

#if defined(_WIN32)
    void* file_handle_ = nullptr;
    void* map_handle_ = nullptr;
#elif !defined(__EMSCRIPTEN__)
    int fd_ = -1;
#else
    std::vector<uint8_t> fallback_buf_;
#endif
};

/// Background Compactor agent (see AGENTS.md).
/// Periodically invokes a compaction callback on a background scheduler
/// to merge the append-only WAL into the compacted main snapshot file.
class Compactor {
public:
    Compactor() = default;
    ~Compactor();

    Compactor(const Compactor&) = delete;
    Compactor& operator=(const Compactor&) = delete;

    void start(uint32_t interval_ms, std::function<void()> compact_fn);
    void trigger_now();
    void stop();

    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void compactor_loop();

    cpp26::execution::thread_pool_scheduler sched_{1};
    std::function<void()> compact_fn_;
    uint32_t interval_ms_{5000};
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    bool trigger_requested_{false};
};

} // namespace zephyr::storage

#endif // ZEPHYR_STORAGE_MMAP_STORE_HPP
