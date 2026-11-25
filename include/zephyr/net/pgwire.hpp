#ifndef ZEPHYR_NET_PGWIRE_HPP
#define ZEPHYR_NET_PGWIRE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zephyr/c_abi.h"
#include "zephyr/compat/cpp26.hpp"

namespace zephyr::net {

/**
 * Lightweight PostgreSQL Wire Protocol v3.0 TCP server.
 * Uses C-ABI zephyr_query / zephyr_result_free / zephyr_last_error
 * to avoid circular dependencies with internal SQL engine headers.
 */
class PgWireServer {
public:
    using QueryHandler = std::function<ZephyrResultSet*(const char* sql)>;
    using ErrorProvider = std::function<const char*()>;

    PgWireServer() = default;
    ~PgWireServer();

    PgWireServer(const PgWireServer&) = delete;
    PgWireServer& operator=(const PgWireServer&) = delete;

    /**
     * Start listening on TCP `port` (pass 0 for ephemeral port) and route
     * SimpleQuery ('Q') messages to `db` via zephyr_query().
     */
    bool start(ZephyrDB* db, uint16_t port);

    /**
     * Start listening on TCP `port` with a custom query handler.
     */
    bool start(QueryHandler handler, uint16_t port, ErrorProvider err_provider = {});

    void stop();

    uint16_t bound_port() const noexcept { return bound_port_.load(std::memory_order_acquire); }
    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    bool start_impl(ZephyrDB* db, QueryHandler handler, uint16_t port, ErrorProvider err_provider);
    void accept_loop();
    void handle_client(uintptr_t client_sock);

    std::atomic<bool> running_{false};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<uintptr_t> listen_sock_{static_cast<uintptr_t>(~0ULL)};
    std::atomic<int32_t> next_backend_pid_{1000};

    ZephyrDB* db_{nullptr};
    QueryHandler query_handler_;
    ErrorProvider error_provider_;

    std::mutex clients_mu_;
    std::vector<uintptr_t> client_socks_;

    std::unique_ptr<zephyr::cpp26::execution::thread_pool_scheduler> sched_;
};

} // namespace zephyr::net

#endif // ZEPHYR_NET_PGWIRE_HPP
