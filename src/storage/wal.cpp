#include "zephyr/storage/wal.hpp"
#include "zephyr/storage/crypto.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <io.h>
  #include <windows.h>
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

void serialize_record(std::vector<uint8_t>& out, uint64_t lsn, uint8_t op, std::string_view payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    write_u32_le(out, WalManager::MAGIC);
    const size_t crc_start = out.size();
    write_u64_le(out, lsn);
    out.push_back(op);
    write_u32_le(out, len);
    if (len > 0) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(payload.data());
        out.insert(out.end(), bytes, bytes + len);
    }
    const uint32_t crc = crc32_update(0, out.data() + crc_start, out.size() - crc_start);
    write_u32_le(out, crc);
}

void serialize_record_encrypted(std::vector<uint8_t>& out, uint64_t lsn, uint8_t op,
                                std::string_view payload,
                                const uint8_t key[32], const uint8_t salt[4]) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const size_t rec_start = out.size();

    // Format: [magic:4][salt:4][lsn:8][op:1][len:4][ct:len][tag:16]
    write_u32_le(out, WalManager::MAGIC_ENCRYPTED);
    out.insert(out.end(), salt, salt + 4);
    write_u64_le(out, lsn);
    out.push_back(op);
    write_u32_le(out, len);

    // nonce = salt(4) || lsn(8)
    uint8_t nonce[12];
    std::memcpy(nonce, out.data() + rec_start + 4, 4);
    std::memcpy(nonce + 4, out.data() + rec_start + 8, 8);

    // aad = magic(4) || lsn(8) || op(1) || len(4)
    uint8_t aad[17];
    std::memcpy(aad, out.data() + rec_start, 4);
    std::memcpy(aad + 4, out.data() + rec_start + 8, 13);

    const size_t ct_offset = out.size();
    out.resize(ct_offset + len + 16);

    const uint8_t* pt = (len > 0) ? reinterpret_cast<const uint8_t*>(payload.data()) : nullptr;
    uint8_t* ct_out = (len > 0) ? (out.data() + ct_offset) : nullptr;
    uint8_t* tag_out = out.data() + ct_offset + len;

    crypto::aead_encrypt(key, nonce, aad, sizeof(aad), pt, len, ct_out, tag_out);
}

void os_flush_file(std::FILE* fp) noexcept {
    if (!fp) {
        return;
    }
#if defined(_WIN32)
    const intptr_t os_handle = _get_osfhandle(_fileno(fp));
    if (os_handle != -1) {
        FlushFileBuffers(reinterpret_cast<HANDLE>(os_handle));
    }
#elif !defined(__EMSCRIPTEN__)
    fsync(fileno(fp));
#else
    (void)fp;
#endif
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
    if (sql.empty()) {
        buffer_[h].sql.clear();
    } else {
        buffer_[h].sql.assign(sql.data(), sql.size());
    }
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

size_t WalRingBuffer::size() const noexcept {
    const size_t h = head_.load(std::memory_order_acquire);
    const size_t t = tail_.load(std::memory_order_acquire);
    return (h - t) & (CAPACITY - 1);
}

// ----------------------------------------------------------------------------
// WalManager (WAL Guardian)
// ----------------------------------------------------------------------------

WalManager::~WalManager() {
    close();
}

void WalManager::set_encryption_key(const uint8_t* key32, size_t len) {
    std::lock_guard<std::mutex> flk(file_mu_);
    if (key32 == nullptr || len == 0) {
        encrypted_ = false;
        enc_key_.fill(0);
        nonce_salt_.fill(0);
        return;
    }
    if (len == 32) {
        std::memcpy(enc_key_.data(), key32, 32);
    } else {
        uint8_t unused_snap[32]{};
        crypto::hkdf_derive_keys(key32, len, enc_key_.data(), unused_snap);
    }
    crypto::random_bytes(nonce_salt_.data(), nonce_salt_.size());
    encrypted_ = true;
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
    synced_lsn_.store(max_lsn, std::memory_order_release);
    group_commit_batches_.store(0, std::memory_order_release);
    group_commit_records_.store(0, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        wake_requested_ = false;
        sync_requested_ = false;
    }

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
        sync_requested_ = true;
    }
    wake_cv_.notify_all();
    flush_cv_.notify_all();
    sched_.stop();
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        sync_requested_ = true;
    }
    drain_and_write();
}

uint64_t WalManager::append(uint8_t op, std::string_view sql, bool sync_wait) {
    if (!open_.load(std::memory_order_acquire)) {
        return 0;
    }

    if (sync_wait) {
        std::lock_guard<std::mutex> clk(cv_mu_);
        sync_requested_ = true;
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
            sync_requested_ = true;
            wake_requested_ = true;
        }
        wake_cv_.notify_one();

        std::unique_lock<std::mutex> lk(cv_mu_);
        flush_cv_.wait(lk, [this, lsn] {
            return synced_lsn_.load(std::memory_order_acquire) >= lsn ||
                   !running_.load(std::memory_order_acquire);
        });
    }
    return lsn;
}

uint64_t WalManager::append_binary(uint8_t op, const uint8_t* payload, size_t len, bool sync_wait) {
    std::string_view view;
    if (payload != nullptr && len > 0) {
        view = std::string_view(reinterpret_cast<const char*>(payload), len);
    }
    return append(op, view, sync_wait);
}

void WalManager::flush() {
    if (!open_.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t target = next_lsn_.load(std::memory_order_acquire) - 1;
    if (synced_lsn_.load(std::memory_order_acquire) >= target && ring_.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        sync_requested_ = true;
        wake_requested_ = true;
    }
    wake_cv_.notify_one();

    std::unique_lock<std::mutex> lk(cv_mu_);
    flush_cv_.wait(lk, [this, target] {
        return (synced_lsn_.load(std::memory_order_acquire) >= target && ring_.empty()) ||
               !running_.load(std::memory_order_acquire);
    });
}

void WalManager::guardian_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(cv_mu_);
            wake_cv_.wait_for(lk, std::chrono::milliseconds(flush_ms_), [this] {
                return !running_.load(std::memory_order_acquire) ||
                       wake_requested_ ||
                       sync_requested_ ||
                       !ring_.empty();
            });
            wake_requested_ = false;
        }
        drain_and_write();
    }
}

void WalManager::drain_and_write() {
    static constexpr size_t kGroupCommitMaxBatch = 64;
    const uint32_t gc_us = group_commit_us_.load(std::memory_order_acquire);
    if (gc_us > 0 && running_.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(cv_mu_);
        if (sync_requested_ && ring_.size() < kGroupCommitMaxBatch) {
            wake_cv_.wait_for(lk, std::chrono::microseconds(gc_us), [this] {
                return !running_.load(std::memory_order_acquire) ||
                       ring_.size() >= kGroupCommitMaxBatch;
            });
        }
    }

    std::vector<WalRingBuffer::Slot> batch;
    ring_.pop_batch(batch, WalRingBuffer::CAPACITY);

    bool do_sync = false;
    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        do_sync = sync_requested_;
        sync_requested_ = false;
    }

    const uint64_t prev_flushed = flushed_lsn_.load(std::memory_order_relaxed);
    const uint64_t prev_synced = synced_lsn_.load(std::memory_order_relaxed);
    if (batch.empty() && (!do_sync || prev_synced >= prev_flushed)) {
        return;
    }

    bool use_enc = false;
    std::array<uint8_t, 32> key_copy{};
    std::array<uint8_t, 4> salt_copy{};
    if (!batch.empty()) {
        std::lock_guard<std::mutex> flk(file_mu_);
        use_enc = encrypted_;
        if (use_enc) {
            key_copy = enc_key_;
            salt_copy = nonce_salt_;
        }
    }

    std::vector<uint8_t> encoded;
    uint64_t max_lsn = prev_flushed;
    if (!batch.empty()) {
        const size_t overhead = use_enc ? 37u : 21u;
        size_t total_bytes = 0;
        for (const auto& s : batch) {
            total_bytes += overhead + s.sql.size();
        }
        encoded.reserve(total_bytes);

        for (const auto& s : batch) {
            if (use_enc) {
                serialize_record_encrypted(encoded, s.lsn, s.op, s.sql,
                                           key_copy.data(), salt_copy.data());
            } else {
                serialize_record(encoded, s.lsn, s.op, s.sql);
            }
            if (s.lsn > max_lsn) {
                max_lsn = s.lsn;
            }
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
            if (do_sync) {
                os_flush_file(fp);
            }
            std::fclose(fp);
        }
    }

    {
        std::lock_guard<std::mutex> lk(cv_mu_);
        flushed_lsn_.store(max_lsn, std::memory_order_release);
        if (do_sync) {
            synced_lsn_.store(max_lsn, std::memory_order_release);
        }
    }
    if (do_sync && !batch.empty()) {
        group_commit_batches_.fetch_add(1, std::memory_order_relaxed);
        group_commit_records_.fetch_add(batch.size(), std::memory_order_relaxed);
    }
    flush_cv_.notify_all();
}

std::vector<WalRecord> WalManager::replay(bool* auth_failed) const {
    if (auth_failed) {
        *auth_failed = false;
    }
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
    while (pos + 4 <= buf.size()) {
        const uint32_t magic = read_u32_le(buf.data() + pos);
        if (magic == MAGIC) {
            if (encrypted_) {
                if (auth_failed) {
                    *auth_failed = true;
                }
                break;
            }
            // Plaintext: 4 (magic) + 8 (lsn) + 1 (op) + 4 (len) + len + 4 (crc) = 21 + len
            if (pos + 21 > buf.size()) {
                break; // partial tail record
            }
            const size_t crc_start = pos + 4;
            const uint64_t lsn = read_u64_le(buf.data() + pos + 4);
            const uint8_t op = buf[pos + 12];
            const uint32_t len = read_u32_le(buf.data() + pos + 13);

            if (len > buf.size() - pos - 21) {
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
        } else if (magic == MAGIC_ENCRYPTED) {
            if (!encrypted_) {
                if (auth_failed) {
                    *auth_failed = true;
                }
                break;
            }
            // Encrypted: [magic:4][salt:4][lsn:8][op:1][len:4][ct:len][tag:16] = 37 + len
            if (pos + 37 > buf.size()) {
                break; // partial tail record
            }
            const uint64_t lsn = read_u64_le(buf.data() + pos + 8);
            const uint8_t op = buf[pos + 16];
            const uint32_t len = read_u32_le(buf.data() + pos + 17);

            if (len > buf.size() - pos - 37) {
                break; // partial tail record
            }

            // nonce = salt(4) || lsn(8)
            uint8_t nonce[12];
            std::memcpy(nonce, buf.data() + pos + 4, 4);
            std::memcpy(nonce + 4, buf.data() + pos + 8, 8);

            // aad = magic(4) || lsn(8) || op(1) || len(4)
            uint8_t aad[17];
            std::memcpy(aad, buf.data() + pos, 4);
            std::memcpy(aad + 4, buf.data() + pos + 8, 13);

            const uint8_t* ct = (len > 0) ? (buf.data() + pos + 21) : nullptr;
            const uint8_t* tag = buf.data() + pos + 21 + len;

            WalRecord rec;
            rec.lsn = lsn;
            rec.op = op;
            rec.sql.resize(len);
            uint8_t* pt_out = (len > 0) ? reinterpret_cast<uint8_t*>(rec.sql.data()) : nullptr;

            if (!crypto::aead_decrypt(enc_key_.data(), nonce, aad, sizeof(aad),
                                      ct, len, tag, pt_out)) {
                if (auth_failed) {
                    *auth_failed = true;
                }
                break;
            }

            records.push_back(std::move(rec));
            pos += 37 + len;
        } else {
            break;
        }
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

    std::lock_guard<std::mutex> flk(file_mu_);
    std::vector<uint8_t> encoded;
    for (const auto& rec : keep) {
        if (encrypted_) {
            serialize_record_encrypted(encoded, rec.lsn, rec.op, rec.sql,
                                       enc_key_.data(), nonce_salt_.data());
        } else {
            serialize_record(encoded, rec.lsn, rec.op, rec.sql);
        }
    }

    std::FILE* fp = std::fopen(wal_path_.c_str(), "wb");
    if (fp) {
        if (!encoded.empty()) {
            std::fwrite(encoded.data(), 1, encoded.size(), fp);
        }
        std::fflush(fp);
        os_flush_file(fp);
        std::fclose(fp);
    }
}

} // namespace zephyr::storage
