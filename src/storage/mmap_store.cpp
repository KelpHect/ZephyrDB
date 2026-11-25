#include "zephyr/storage/mmap_store.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <fcntl.h>
  #include <sys/mman.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace zephyr::storage {

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
