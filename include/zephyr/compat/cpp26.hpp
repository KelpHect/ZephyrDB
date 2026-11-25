#ifndef ZEPHYR_COMPAT_CPP26_HPP
#define ZEPHYR_COMPAT_CPP26_HPP

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__AVX2__) || defined(__SSE4_2__) || (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_AMD64)))
  #if !defined(__EMSCRIPTEN__)
    #include <immintrin.h>
    #define ZEPHYR_HAS_X86_SIMD 1
  #endif
#endif

#if defined(__EMSCRIPTEN__) && defined(__wasm_simd128__)
  #include <wasm_simd128.h>
  #define ZEPHYR_HAS_WASM_SIMD 1
#endif

// Feature detection for native C++26 headers
#if defined(__has_include)
  #if __has_include(<rcu>) && defined(__cpp_lib_rcu)
    #include <rcu>
    #define ZEPHYR_NATIVE_RCU 1
  #endif
  #if __has_include(<simd>) && defined(__cpp_lib_simd)
    #include <simd>
    #define ZEPHYR_NATIVE_SIMD 1
  #endif
  #if __has_include(<execution>) && defined(__cpp_lib_senders_receivers)
    #include <execution>
    #define ZEPHYR_NATIVE_EXECUTION 1
  #endif
  #if __has_include(<meta>) && defined(__cpp_impl_reflection)
    #include <meta>
    #define ZEPHYR_NATIVE_REFLECTION 1
  #endif
#endif

namespace zephyr::cpp26 {

// ============================================================================
// 1. C++26 RCU (P2545) Compatibility Layer
// ============================================================================
namespace rcu {

struct retired_node {
    void* ptr = nullptr;
    void (*deleter)(void*) = nullptr;
    uint64_t retire_epoch = 0;
};

class rcu_domain {
public:
    static constexpr size_t MAX_READER_SLOTS = 256;

    struct alignas(64) reader_slot {
        std::atomic<bool> in_use{false};
        std::atomic<uint64_t> epoch{0}; // bit 0 == 1 means active reader
    };

    rcu_domain() = default;
    ~rcu_domain() {
        drain_all();
    }

    rcu_domain(const rcu_domain&) = delete;
    rcu_domain& operator=(const rcu_domain&) = delete;

    reader_slot* acquire_thread_slot() noexcept {
        for (auto& slot : slots_) {
            bool expected = false;
            if (slot.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                return &slot;
            }
        }
        return nullptr;
    }

    void release_thread_slot(reader_slot* slot) noexcept {
        if (slot) {
            slot->epoch.store(0, std::memory_order_release);
            slot->in_use.store(false, std::memory_order_release);
        }
    }

    uint64_t enter_read(reader_slot* slot) noexcept {
        uint64_t g = global_epoch_.load(std::memory_order_acquire);
        if (slot) {
            slot->epoch.store((g << 1) | 1ULL, std::memory_order_seq_cst);
        } else {
            fallback_readers_.fetch_add(1, std::memory_order_seq_cst);
        }
        return g;
    }

    void exit_read(reader_slot* slot) noexcept {
        if (slot) {
            slot->epoch.store(0, std::memory_order_release);
        } else {
            fallback_readers_.fetch_sub(1, std::memory_order_release);
        }
    }

    void retire(void* ptr, void (*deleter)(void*)) {
        if (!ptr || !deleter) return;
        uint64_t e = global_epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::lock_guard<std::mutex> lk(retire_mu_);
        retired_.push_back(retired_node{ptr, deleter, e});
    }

    size_t poll_reclaim() {
        std::vector<retired_node> ready;
        uint64_t min_active = min_active_epoch();
        {
            std::lock_guard<std::mutex> lk(retire_mu_);
            if (retired_.empty()) return 0;
            size_t write_idx = 0;
            for (size_t i = 0; i < retired_.size(); ++i) {
                if (retired_[i].retire_epoch < min_active) {
                    ready.push_back(retired_[i]);
                } else {
                    retired_[write_idx++] = retired_[i];
                }
            }
            retired_.resize(write_idx);
        }
        for (auto& n : ready) {
            n.deleter(n.ptr);
        }
        return ready.size();
    }

    void synchronize() {
        uint64_t target = global_epoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
        while (min_active_epoch() <= target) {
            std::this_thread::yield();
        }
        poll_reclaim();
    }

    size_t pending_count() const {
        std::lock_guard<std::mutex> lk(retire_mu_);
        return retired_.size();
    }

    void drain_all() {
        std::vector<retired_node> all;
        {
            std::lock_guard<std::mutex> lk(retire_mu_);
            all.swap(retired_);
        }
        for (auto& n : all) {
            n.deleter(n.ptr);
        }
    }

private:
    uint64_t min_active_epoch() const noexcept {
        while (fallback_readers_.load(std::memory_order_acquire) > 0) {
            std::this_thread::yield();
        }
        uint64_t g = global_epoch_.load(std::memory_order_seq_cst);
        uint64_t min_e = g + 1;
        for (const auto& slot : slots_) {
            if (!slot.in_use.load(std::memory_order_acquire)) continue;
            uint64_t val = slot.epoch.load(std::memory_order_seq_cst);
            if (val & 1ULL) {
                uint64_t e = val >> 1;
                if (e < min_e) min_e = e;
            }
        }
        return min_e;
    }

    std::atomic<uint64_t> global_epoch_{1};
    std::atomic<uint64_t> fallback_readers_{0};
    std::array<reader_slot, MAX_READER_SLOTS> slots_{};
    mutable std::mutex retire_mu_;
    std::vector<retired_node> retired_;
};

inline rcu_domain& default_domain() {
    static rcu_domain dom;
    return dom;
}

class scoped_read_lock {
public:
    explicit scoped_read_lock(rcu_domain& dom = default_domain()) noexcept
        : domain_(&dom) {
        slot_ = get_local_slot(dom);
        domain_->enter_read(slot_);
    }

    ~scoped_read_lock() {
        if (domain_) {
            domain_->exit_read(slot_);
        }
    }

    scoped_read_lock(const scoped_read_lock&) = delete;
    scoped_read_lock& operator=(const scoped_read_lock&) = delete;

private:
    static rcu_domain::reader_slot* get_local_slot(rcu_domain& dom) noexcept {
        struct slot_holder {
            rcu_domain* dom = nullptr;
            rcu_domain::reader_slot* slot = nullptr;
            ~slot_holder() {
                if (dom && slot) dom->release_thread_slot(slot);
            }
        };
        thread_local slot_holder holder;
        if (holder.dom == &dom && holder.slot) {
            return holder.slot;
        }
        if (holder.dom == nullptr) {
            holder.dom = &dom;
            holder.slot = dom.acquire_thread_slot();
            return holder.slot;
        }
        return nullptr;
    }

    rcu_domain* domain_;
    rcu_domain::reader_slot* slot_;
};

template <typename T>
void default_deleter(void* p) {
    delete static_cast<T*>(p);
}

template <typename T>
inline void rcu_retire(T* ptr, rcu_domain& dom = default_domain()) {
    dom.retire(ptr, &default_deleter<T>);
}

} // namespace rcu

// ============================================================================
// 2. C++26 SIMD (P1928) Compatibility Layer
// ============================================================================
namespace simd {

enum class cmp_op : uint8_t { eq = 0, ne = 1, lt = 2, le = 3, gt = 4, ge = 5 };

template <typename T>
inline bool eval_scalar_cmp(T a, T b, cmp_op op) noexcept {
    switch (op) {
        case cmp_op::eq: return a == b;
        case cmp_op::ne: return a != b;
        case cmp_op::lt: return a < b;
        case cmp_op::le: return a <= b;
        case cmp_op::gt: return a > b;
        case cmp_op::ge: return a >= b;
    }
    return false;
}

// Vectorized 64-bit integer comparison returning bitmask for up to 64 elements
inline uint64_t filter_chunk_i64(const int64_t* data, size_t count, int64_t rhs, cmp_op op) noexcept {
    uint64_t mask = 0;
    size_t i = 0;

#if defined(ZEPHYR_HAS_X86_SIMD) && (defined(__AVX2__) || defined(_MSC_VER))
    // Use AVX2 when available at compile time (/arch:AVX2 or -mavx2)
  #if defined(__AVX2__)
    const __m256i vrhs = _mm256_set1_epi64x(rhs);
    for (; i + 4 <= count && i < 64; i += 4) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        __m256i cmp = _mm256_setzero_si256();
        switch (op) {
            case cmp_op::eq: cmp = _mm256_cmpeq_epi64(v, vrhs); break;
            case cmp_op::ne: cmp = _mm256_xor_si256(_mm256_cmpeq_epi64(v, vrhs), _mm256_set1_epi64x(-1)); break;
            case cmp_op::gt: cmp = _mm256_cmpgt_epi64(v, vrhs); break;
            case cmp_op::lt: cmp = _mm256_cmpgt_epi64(vrhs, v); break;
            case cmp_op::ge: cmp = _mm256_xor_si256(_mm256_cmpgt_epi64(vrhs, v), _mm256_set1_epi64x(-1)); break;
            case cmp_op::le: cmp = _mm256_xor_si256(_mm256_cmpgt_epi64(v, vrhs), _mm256_set1_epi64x(-1)); break;
        }
        uint32_t m = static_cast<uint32_t>(_mm256_movemask_pd(_mm256_castsi256_pd(cmp)));
        mask |= (static_cast<uint64_t>(m) << i);
    }
  #endif
#elif defined(ZEPHYR_HAS_WASM_SIMD)
    v128_t vrhs = wasm_i64x2_splat(rhs);
    for (; i + 2 <= count && i < 64; i += 2) {
        v128_t v = wasm_v128_load(data + i);
        v128_t cmp;
        switch (op) {
            case cmp_op::eq: cmp = wasm_i64x2_eq(v, vrhs); break;
            case cmp_op::ne: cmp = wasm_i64x2_ne(v, vrhs); break;
            case cmp_op::lt: cmp = wasm_i64x2_lt(v, vrhs); break;
            case cmp_op::le: cmp = wasm_i64x2_le(v, vrhs); break;
            case cmp_op::gt: cmp = wasm_i64x2_gt(v, vrhs); break;
            case cmp_op::ge: cmp = wasm_i64x2_ge(v, vrhs); break;
        }
        uint32_t m = static_cast<uint32_t>(wasm_i64x2_bitmask(cmp));
        mask |= (static_cast<uint64_t>(m) << i);
    }
#endif

    for (; i < count && i < 64; ++i) {
        if (eval_scalar_cmp(data[i], rhs, op)) {
            mask |= (1ULL << i);
        }
    }
    return mask;
}

inline uint64_t filter_chunk_f64(const double* data, size_t count, double rhs, cmp_op op) noexcept {
    uint64_t mask = 0;
    size_t i = 0;

#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)
    const __m256d vrhs = _mm256_set1_pd(rhs);
    for (; i + 4 <= count && i < 64; i += 4) {
        __m256d v = _mm256_loadu_pd(data + i);
        __m256d cmp = _mm256_setzero_pd();
        switch (op) {
            case cmp_op::eq: cmp = _mm256_cmp_pd(v, vrhs, _CMP_EQ_OQ); break;
            case cmp_op::ne: cmp = _mm256_cmp_pd(v, vrhs, _CMP_NEQ_UQ); break;
            case cmp_op::lt: cmp = _mm256_cmp_pd(v, vrhs, _CMP_LT_OQ); break;
            case cmp_op::le: cmp = _mm256_cmp_pd(v, vrhs, _CMP_LE_OQ); break;
            case cmp_op::gt: cmp = _mm256_cmp_pd(v, vrhs, _CMP_GT_OQ); break;
            case cmp_op::ge: cmp = _mm256_cmp_pd(v, vrhs, _CMP_GE_OQ); break;
        }
        uint32_t m = static_cast<uint32_t>(_mm256_movemask_pd(cmp));
        mask |= (static_cast<uint64_t>(m) << i);
    }
#elif defined(ZEPHYR_HAS_WASM_SIMD)
    v128_t vrhs = wasm_f64x2_splat(rhs);
    for (; i + 2 <= count && i < 64; i += 2) {
        v128_t v = wasm_v128_load(data + i);
        v128_t cmp;
        switch (op) {
            case cmp_op::eq: cmp = wasm_f64x2_eq(v, vrhs); break;
            case cmp_op::ne: cmp = wasm_f64x2_ne(v, vrhs); break;
            case cmp_op::lt: cmp = wasm_f64x2_lt(v, vrhs); break;
            case cmp_op::le: cmp = wasm_f64x2_le(v, vrhs); break;
            case cmp_op::gt: cmp = wasm_f64x2_gt(v, vrhs); break;
            case cmp_op::ge: cmp = wasm_f64x2_ge(v, vrhs); break;
        }
        uint32_t m = static_cast<uint32_t>(wasm_i64x2_bitmask(cmp));
        mask |= (static_cast<uint64_t>(m) << i);
    }
#endif

    for (; i < count && i < 64; ++i) {
        if (eval_scalar_cmp(data[i], rhs, op)) {
            mask |= (1ULL << i);
        }
    }
    return mask;
}

inline int64_t reduce_sum_i64(const int64_t* data, size_t count) noexcept {
    int64_t acc = 0;
    size_t i = 0;
#if defined(ZEPHYR_HAS_X86_SIMD) && defined(__AVX2__)
    __m256i vacc = _mm256_setzero_si256();
    for (; i + 4 <= count; i += 4) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        vacc = _mm256_add_epi64(vacc, v);
    }
    alignas(32) int64_t tmp[4];
    _mm256_store_si256(reinterpret_cast<__m256i*>(tmp), vacc);
    acc = tmp[0] + tmp[1] + tmp[2] + tmp[3];
#endif
    for (; i < count; ++i) acc += data[i];
    return acc;
}

} // namespace simd

// ============================================================================
// 3. C++26 std::execution (P2300 Senders/Receivers) Compatibility Layer
// ============================================================================
namespace execution {

class thread_pool_scheduler {
public:
    explicit thread_pool_scheduler(size_t threads = 2) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~thread_pool_scheduler() {
        stop();
    }

    thread_pool_scheduler(const thread_pool_scheduler&) = delete;
    thread_pool_scheduler& operator=(const thread_pool_scheduler&) = delete;

    void enqueue(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return;
            tasks_.push(std::move(fn));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_) return;
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
    }

private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stopping_ || !tasks_.empty(); });
                if (stopping_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            if (task) task();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

template <typename Fn>
struct task_sender {
    thread_pool_scheduler* sched;
    Fn fn;

    using result_type = std::invoke_result_t<Fn>;

    template <typename NextFn>
    auto then(NextFn next) && {
        auto prev = std::move(fn);
        auto* s = sched;
        return task_sender<decltype([prev = std::move(prev), next = std::move(next)]() mutable {
            if constexpr (std::is_void_v<result_type>) {
                prev();
                return next();
            } else {
                return next(prev());
            }
        })>{s, [prev = std::move(prev), next = std::move(next)]() mutable {
            if constexpr (std::is_void_v<result_type>) {
                prev();
                return next();
            } else {
                return next(prev());
            }
        }};
    }
};

template <typename Fn>
inline auto schedule_task(thread_pool_scheduler& sched, Fn&& fn) {
    using Decayed = std::decay_t<Fn>;
    return task_sender<Decayed>{&sched, std::forward<Fn>(fn)};
}

template <typename Sender>
inline auto sync_wait(Sender&& snd) {
    using R = typename std::decay_t<Sender>::result_type;
    std::promise<R> prom;
    auto fut = prom.get_future();
    snd.sched->enqueue([fn = std::move(snd.fn), p = std::move(prom)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                p.set_value();
            } else {
                p.set_value(fn());
            }
        } catch (...) {
            p.set_exception(std::current_exception());
        }
    });
    return fut.get();
}

template <typename Sender>
inline void start_detached(Sender&& snd) {
    snd.sched->enqueue([fn = std::move(snd.fn)]() mutable { fn(); });
}

} // namespace execution

// ============================================================================
// 4. C++26 Static Reflection (P2996 <meta>) Compatibility Layer
// ============================================================================
namespace reflection {

template <typename ClassT, typename MemberT>
struct field_descriptor {
    const char* name;
    MemberT ClassT::* ptr;
};

template <typename ClassT, typename MemberT>
constexpr field_descriptor<ClassT, MemberT> make_field(const char* name, MemberT ClassT::* ptr) noexcept {
    return {name, ptr};
}

template <typename T, typename Fn>
constexpr void for_each_field(const T& obj, Fn&& fn) {
    constexpr auto fields = zephyr_reflect_fields(static_cast<T*>(nullptr));
    std::apply([&](const auto&... f) {
        (fn(f.name, obj.*(f.ptr)), ...);
    }, fields);
}

template <typename T>
inline std::string to_json(const T& obj) {
    std::ostringstream os;
    os << '{';
    bool first = true;
    for_each_field(obj, [&](const char* name, const auto& val) {
        if (!first) os << ',';
        first = false;
        os << '"' << name << "\":";
        using V = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<V, bool>) {
            os << (val ? "true" : "false");
        } else if constexpr (std::is_arithmetic_v<V>) {
            os << val;
        } else {
            os << '"';
            for (char c : std::string_view(val)) {
                if (c == '"' || c == '\\') os << '\\';
                os << c;
            }
            os << '"';
        }
    });
    os << '}';
    return os.str();
}

} // namespace reflection

} // namespace zephyr::cpp26

#define ZEPHYR_FIELD(Type, Member) ::zephyr::cpp26::reflection::make_field(#Member, &Type::Member)
#define ZEPHYR_REFLECT(Type, ...) \
    inline constexpr auto zephyr_reflect_fields(Type*) noexcept { \
        return std::make_tuple(__VA_ARGS__); \
    }

#endif // ZEPHYR_COMPAT_CPP26_HPP
