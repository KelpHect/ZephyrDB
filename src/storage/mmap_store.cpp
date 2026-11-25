#include "zephyr/storage/mmap_store.hpp"
#include "zephyr/storage/crypto.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

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
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace zephyr::storage {

namespace {

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

MmapStore::~MmapStore() {
    close_mmap();
}

MmapStore::MmapStore(MmapStore&& other) noexcept
    : data_(other.data_),
      size_(other.size_)
#if defined(_WIN32)
    , file_handle_(other.file_handle_),
      map_handle_(other.map_handle_)
#elif !defined(__EMSCRIPTEN__)
    , fd_(other.fd_)
#else
    , fallback_buf_(std::move(other.fallback_buf_))
#endif
{
    other.data_ = nullptr;
    other.size_ = 0;
#if defined(_WIN32)
    other.file_handle_ = nullptr;
    other.map_handle_ = nullptr;
#elif !defined(__EMSCRIPTEN__)
    other.fd_ = -1;
#endif
}

MmapStore& MmapStore::operator=(MmapStore&& other) noexcept {
    if (this != &other) {
        close_mmap();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
#if defined(_WIN32)
        file_handle_ = other.file_handle_;
        map_handle_ = other.map_handle_;
        other.file_handle_ = nullptr;
        other.map_handle_ = nullptr;
#elif !defined(__EMSCRIPTEN__)
        fd_ = other.fd_;
        other.fd_ = -1;
#else
        fallback_buf_ = std::move(other.fallback_buf_);
#endif
    }
    return *this;
}

bool MmapStore::write_snapshot(const std::string& path, const std::vector<uint8_t>& blob) {
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream os(tmp_path, std::ios::binary | std::ios::trunc);
        if (!os.is_open()) {
            return false;
        }
        if (!blob.empty()) {
            os.write(reinterpret_cast<const char*>(blob.data()),
                     static_cast<std::streamsize>(blob.size()));
        }
        os.flush();
        if (!os.good()) {
            return false;
        }
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
#else
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
#endif
}

bool MmapStore::write_snapshot_encrypted(const std::string& path,
                                         const std::vector<uint8_t>& blob,
                                         const uint8_t key32[32]) {
    if (!key32) {
        return false;
    }

    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    // Format: [magic:4][nonce:12][len:8][ct:len][tag:16]
    const size_t pt_len = blob.size();
    std::vector<uint8_t> out(40 + pt_len);

    store_le32(out.data(), SNAP_ENC_MAGIC);
    crypto::random_bytes(out.data() + 4, 12);
    store_le64(out.data() + 16, static_cast<uint64_t>(pt_len));

    const uint8_t* nonce = out.data() + 4;
    const uint8_t* aad = out.data(); // 24 bytes: magic(4) || nonce(12) || len(8)
    const uint8_t* pt = pt_len > 0 ? blob.data() : nullptr;
    uint8_t* ct = pt_len > 0 ? (out.data() + 24) : nullptr;
    uint8_t* tag = out.data() + 24 + pt_len;

    crypto::aead_encrypt(key32, nonce, aad, 24, pt, pt_len, ct, tag);

    const std::string tmp_path = path + ".tmp";
    {
        std::FILE* fp = std::fopen(tmp_path.c_str(), "wb");
        if (!fp) {
            return false;
        }
        const size_t written = std::fwrite(out.data(), 1, out.size(), fp);
        const int flush_rc = std::fflush(fp);
        os_flush_file(fp);
        std::fclose(fp);
        if (written != out.size() || flush_rc != 0) {
            std::filesystem::remove(tmp_path, ec);
            return false;
        }
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
#else
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
#endif
}

bool MmapStore::read_snapshot_encrypted(const std::string& path,
                                        const uint8_t key32[32],
                                        std::vector<uint8_t>& out_blob,
                                        bool* auth_failed) {
    if (auth_failed) {
        *auth_failed = false;
    }
    if (!key32) {
        if (auth_failed) {
            *auth_failed = true;
        }
        return false;
    }

    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        return false;
    }

    std::fseek(fp, 0, SEEK_END);
    const long fsize = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (fsize < 0) {
        std::fclose(fp);
        return false;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(fsize));
    const size_t read_bytes = (fsize > 0) ? std::fread(buf.data(), 1, buf.size(), fp) : 0;
    std::fclose(fp);
    buf.resize(read_bytes);

    // Minimum size: [magic:4][nonce:12][len:8][tag:16] = 40 bytes
    if (buf.size() < 40) {
        if (auth_failed) {
            *auth_failed = true;
        }
        return false;
    }

    const uint32_t magic = load_le32(buf.data());
    if (magic != SNAP_ENC_MAGIC) {
        if (auth_failed) {
            *auth_failed = true;
        }
        return false;
    }

    const uint64_t len = load_le64(buf.data() + 16);
    if (len > buf.size() - 40 || 40 + len != buf.size()) {
        if (auth_failed) {
            *auth_failed = true;
        }
        return false;
    }

    const uint8_t* nonce = buf.data() + 4;
    const uint8_t* aad = buf.data(); // 24 bytes: magic(4) || nonce(12) || len(8)
    const uint8_t* ct = len > 0 ? (buf.data() + 24) : nullptr;
    const uint8_t* tag = buf.data() + 24 + static_cast<size_t>(len);

    std::vector<uint8_t> pt(static_cast<size_t>(len));
    uint8_t* pt_out = pt.empty() ? nullptr : pt.data();
    if (!crypto::aead_decrypt(key32, nonce, aad, 24, ct, static_cast<size_t>(len), tag, pt_out)) {
        if (auth_failed) {
            *auth_failed = true;
        }
        return false;
    }

    out_blob = std::move(pt);
    return true;
}

bool MmapStore::open_mmap(const std::string& path) {
    close_mmap();

#if defined(_WIN32)
    HANDLE hFile = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fsize{};
    if (!GetFileSizeEx(hFile, &fsize) || fsize.QuadPart <= 0) {
        CloseHandle(hFile);
        return false;
    }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return false;
    }

    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return false;
    }

    file_handle_ = hFile;
    map_handle_ = hMap;
    data_ = static_cast<const uint8_t*>(view);
    size_ = static_cast<size_t>(fsize.QuadPart);
    return true;

#elif !defined(__EMSCRIPTEN__)
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return false;
    }

    void* mapped = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    fd_ = fd;
    data_ = static_cast<const uint8_t*>(mapped);
    size_ = static_cast<size_t>(st.st_size);
    return true;

#else
    std::ifstream is(path, std::ios::binary | std::ios::ate);
    if (!is.is_open()) {
        return false;
    }
    const auto sz = is.tellg();
    if (sz <= 0) {
        return false;
    }
    fallback_buf_.resize(static_cast<size_t>(sz));
    is.seekg(0, std::ios::beg);
    is.read(reinterpret_cast<char*>(fallback_buf_.data()), sz);
    data_ = fallback_buf_.data();
    size_ = fallback_buf_.size();
    return true;
#endif
}

void MmapStore::close_mmap() {
#if defined(_WIN32)
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (map_handle_) {
        CloseHandle(static_cast<HANDLE>(map_handle_));
        map_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(static_cast<HANDLE>(file_handle_));
        file_handle_ = nullptr;
    }
#elif !defined(__EMSCRIPTEN__)
    if (data_ && size_ > 0) {
        ::munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#else
    fallback_buf_.clear();
    data_ = nullptr;
#endif
    size_ = 0;
}

// ----------------------------------------------------------------------------
// Compactor Agent
// ----------------------------------------------------------------------------

Compactor::~Compactor() {
    stop();
}

void Compactor::start(uint32_t interval_ms, std::function<void()> compact_fn) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    interval_ms_ = (interval_ms == 0) ? 100 : interval_ms;
    compact_fn_ = std::move(compact_fn);

    cpp26::execution::start_detached(
        cpp26::execution::schedule_task(sched_, [this] { compactor_loop(); })
    );
}

void Compactor::trigger_now() {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        trigger_requested_ = true;
    }
    cv_.notify_one();
}

void Compactor::stop() {
    if (running_.exchange(false, std::memory_order_acq_rel)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            trigger_requested_ = true;
        }
        cv_.notify_all();
    }
    sched_.stop();
}

void Compactor::compactor_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this] {
                return !running_.load(std::memory_order_acquire) || trigger_requested_;
            });
            trigger_requested_ = false;
        }
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (compact_fn_) {
            compact_fn_();
        }
    }
}

} // namespace zephyr::storage
