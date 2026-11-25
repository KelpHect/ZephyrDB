#include "zephyr/core/rcu.hpp"

#include <chrono>

namespace zephyr::core {

RcuReaper::RcuReaper(cpp26::rcu::rcu_domain& dom)
    : domain_(dom) {}

RcuReaper::~RcuReaper() {
    stop();
}

void RcuReaper::start(uint32_t interval_ms) {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    interval_ms_ = (interval_ms == 0) ? 1 : interval_ms;
    cpp26::execution::start_detached(
        cpp26::execution::schedule_task(sched_, [this] { reaper_loop(); })
    );
}

void RcuReaper::stop() {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            wake_requested_ = true;
        }
        cv_.notify_all();
    }
    sched_.stop();

    const size_t remaining = domain_.pending_count();
    if (remaining > 0) {
        domain_.drain_all();
        reclaimed_total_.fetch_add(remaining, std::memory_order_relaxed);
    }
}

void RcuReaper::retire(void* ptr, void (*deleter)(void*)) {
    if (!ptr || !deleter) {
        return;
    }
    domain_.retire(ptr, deleter);
    if (running_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            wake_requested_ = true;
        }
        cv_.notify_one();
    }
}

size_t RcuReaper::reclaim_now() {
    const size_t reclaimed = domain_.poll_reclaim();
    if (reclaimed > 0) {
        reclaimed_total_.fetch_add(reclaimed, std::memory_order_relaxed);
    }
    return reclaimed;
}

size_t RcuReaper::retired_count() const {
    return domain_.pending_count();
}

void RcuReaper::reaper_loop() {
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this] {
                return !running_.load(std::memory_order_acquire) || wake_requested_;
            });
            wake_requested_ = false;
        }
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        reclaim_now();
    }
}

} // namespace zephyr::core
