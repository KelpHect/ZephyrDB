#ifndef ZEPHYR_NET_STUDIO_HPP
#define ZEPHYR_NET_STUDIO_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "zephyr/c_abi.h"
#include "zephyr/compat/cpp26.hpp"

namespace zephyr::net {

/**
 * Embedded HTTP/1.1 server for Zephyr Studio Web UI & JSON Telemetry API.
 * Uses C++26 thread_pool_scheduler and pure C-ABI (zephyr_query,
 * zephyr_result_free, zephyr_aggregate_count, zephyr_get_stats,
 * zephyr_checkpoint, zephyr_last_error) with zero external dependencies.
 */
class StudioServer {
public:
    StudioServer() = default;
    ~StudioServer();

    StudioServer(const StudioServer&) = delete;
    StudioServer& operator=(const StudioServer&) = delete;

    /**
     * Start listening on TCP `port` (pass 0 for ephemeral port) and serve
     * the Zephyr Studio Web UI and /api/* JSON endpoints against `db`.
     */
    bool start(ZephyrDB* db, uint16_t port);

    void stop();

    uint16_t bound_port() const noexcept { return bound_port_.load(std::memory_order_acquire); }
    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void accept_loop();
    void handle_client(uintptr_t client_sock);
    void remove_client(uintptr_t client_sock);

    std::atomic<bool> running_{false};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<uintptr_t> listen_sock_{static_cast<uintptr_t>(~0ULL)};

    ZephyrDB* db_{nullptr};

    std::mutex clients_mu_;
    std::vector<uintptr_t> client_socks_;

    std::unique_ptr<zephyr::cpp26::execution::thread_pool_scheduler> sched_;
};

} // namespace zephyr::net

#endif // ZEPHYR_NET_STUDIO_HPP
