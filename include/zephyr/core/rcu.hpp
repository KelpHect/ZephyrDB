#ifndef ZEPHYR_CORE_RCU_HPP
#define ZEPHYR_CORE_RCU_HPP

#include "zephyr/compat/cpp26.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace zephyr::core {

/// Background RCU Reaper agent (see AGENTS.md).
/// Periodically wakes up on a cpp26::execution::thread_pool_scheduler to reclaim
/// retired objects whose grace periods have elapsed, and drains all on stop().
class RcuReaper {
public:
    explicit RcuReaper(cpp26::rcu::rcu_domain& dom = cpp26::rcu::default_domain());
    ~RcuReaper();

    RcuReaper(const RcuReaper&) = delete;
    RcuReaper& operator=(const RcuReaper&) = delete;
    RcuReaper(RcuReaper&&) = delete;
    RcuReaper& operator=(RcuReaper&&) = delete;

    /// Start the background reaper loop with the given wake interval (ms).
    void start(uint32_t interval_ms = 2);

    /// Signal the reaper to stop, join its worker, and drain all remaining retired nodes.
    void stop();

    /// Retire an arbitrary pointer with a custom deleter callback.
    void retire(void* ptr, void (*deleter)(void*));

    /// Retire a typed object using `delete`.
    template <typename T>
    void retire_obj(T* p) {
        if (!p) return;
        retire(p, &cpp26::rcu::default_deleter<T>);
    }

    /// Synchronously poll and reclaim any eligible retired nodes right now.
    size_t reclaim_now();

    size_t retired_count() const;
    uint64_t reclaimed_total() const noexcept {
        return reclaimed_total_.load(std::memory_order_relaxed);
    }

    cpp26::rcu::rcu_domain& domain() noexcept { return domain_; }

private:
    void reaper_loop();

    cpp26::rcu::rcu_domain& domain_;
    cpp26::execution::thread_pool_scheduler sched_{1};
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    bool wake_requested_{false};
    uint32_t interval_ms_{2};
    std::atomic<uint64_t> reclaimed_total_{0};
};

} // namespace zephyr::core

#endif // ZEPHYR_CORE_RCU_HPP
