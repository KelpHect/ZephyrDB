#ifndef ZEPHYR_NET_RAFT_HPP
#define ZEPHYR_NET_RAFT_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zephyr/compat/cpp26.hpp"

namespace zephyr::net {

enum class RaftState : uint8_t {
    Follower = 0,
    Candidate = 1,
    Leader = 2
};

struct RaftLogEntry {
    uint64_t term = 0;
    uint64_t lsn = 0;
    std::string sql;
};

struct RequestVoteArgs {
    uint64_t term = 0;
    uint32_t candidate_id = 0;
    uint64_t last_log_index = 0;
    uint64_t last_log_term = 0;
};

struct RequestVoteReply {
    uint64_t term = 0;
    bool vote_granted = false;
};

struct AppendEntriesArgs {
    uint64_t term = 0;
    uint32_t leader_id = 0;
    uint64_t prev_log_index = 0;
    uint64_t prev_log_term = 0;
    uint64_t leader_commit = 0;
    std::vector<RaftLogEntry> entries;
};

struct AppendEntriesReply {
    uint64_t term = 0;
    uint64_t match_index = 0;
    bool success = false;
};

struct RaftPeer {
    uint32_t id = 0;
    std::string host;
    uint16_t port = 0;
};

class RaftNode;

/**
 * Deterministic in-process transport bus for multi-node Raft unit testing
 * without OS port allocation or socket flakiness.
 */
class InProcessClusterBus {
public:
    InProcessClusterBus() = default;
    ~InProcessClusterBus() = default;

    InProcessClusterBus(const InProcessClusterBus&) = delete;
    InProcessClusterBus& operator=(const InProcessClusterBus&) = delete;

    void register_node(uint32_t node_id, RaftNode* node);
    void unregister_node(uint32_t node_id);

    void set_partition(uint32_t a, uint32_t b, bool partitioned);

    bool send_request_vote(uint32_t from_id, uint32_t to_id,
                           const RequestVoteArgs& args, RequestVoteReply& reply);
    bool send_append_entries(uint32_t from_id, uint32_t to_id,
                             const AppendEntriesArgs& args, AppendEntriesReply& reply);

private:
    static uint64_t link_key(uint32_t a, uint32_t b) noexcept {
        if (a > b) std::swap(a, b);
        return (static_cast<uint64_t>(a) << 32) | static_cast<uint64_t>(b);
    }

    mutable std::mutex mu_;
    std::unordered_map<uint32_t, RaftNode*> nodes_;
    std::unordered_map<uint64_t, bool> partitioned_links_;
};

/**
 * Raft Diplomat Agent:
 * Runs asynchronously on cpp26::execution::thread_pool_scheduler, managing
 * leader election, heartbeats, and WAL byte-delta replication.
 */
class RaftNode {
public:
    using ApplyCallback = std::function<void(uint64_t lsn, std::string_view sql)>;

    RaftNode();
    ~RaftNode();

    RaftNode(const RaftNode&) = delete;
    RaftNode& operator=(const RaftNode&) = delete;

    /**
     * Start Raft Diplomat using UDP socket transport.
     * peers_csv format: "1@127.0.0.1:7001,2@127.0.0.1:7002" or "127.0.0.1:7001,..."
     * Pass port = 0 to bind an ephemeral UDP port (query via bound_port()).
     */
    bool start(uint32_t node_id, uint16_t port, std::string_view peers_csv,
               ApplyCallback apply_cb = {});

    /**
     * Start Raft Diplomat using the in-process cluster bus.
     */
    bool start_in_process(uint32_t node_id, InProcessClusterBus& bus,
                          const std::vector<uint32_t>& peer_ids,
                          ApplyCallback apply_cb = {});

    void stop();

    RaftState state() const;
    uint32_t leader_id() const;
    uint64_t current_term() const;
    uint64_t commit_index() const;
    uint32_t node_id() const noexcept { return node_id_.load(std::memory_order_acquire); }
    uint16_t bound_port() const noexcept { return bound_port_.load(std::memory_order_acquire); }
    bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

    bool propose_wal_entry(uint64_t lsn, std::string_view sql);

    RequestVoteReply handle_request_vote(const RequestVoteArgs& args);
    AppendEntriesReply handle_append_entries(const AppendEntriesArgs& args);

    static std::vector<RaftPeer> parse_peers(std::string_view peers_csv, uint32_t self_id);

private:
    void timer_loop();
    void udp_recv_loop();
    void start_election_locked(std::unique_lock<std::mutex>& lk);
    void send_heartbeats_locked(std::unique_lock<std::mutex>& lk);
    void advance_commit_locked(std::vector<RaftLogEntry>& to_apply);
    void invoke_apply(const std::vector<RaftLogEntry>& entries);

    uint64_t last_log_index_locked() const noexcept;
    uint64_t last_log_term_locked() const noexcept;
    std::chrono::milliseconds random_election_timeout();

    bool rpc_request_vote(const RaftPeer& peer, const RequestVoteArgs& args, RequestVoteReply& reply);
    bool rpc_append_entries(const RaftPeer& peer, const AppendEntriesArgs& args, AppendEntriesReply& reply);

    std::atomic<bool> running_{false};
    std::atomic<uint32_t> node_id_{0};
    std::atomic<uint16_t> bound_port_{0};
    std::atomic<RaftState> state_atomic_{RaftState::Follower};
    std::atomic<uint32_t> leader_id_atomic_{0};
    std::atomic<uint64_t> term_atomic_{0};
    std::atomic<uint64_t> commit_atomic_{0};

    mutable std::mutex mu_;
    RaftState state_{RaftState::Follower};
    uint32_t leader_id_{0};
    uint64_t current_term_{0};
    uint32_t voted_for_{0};
    uint64_t commit_index_{0};
    uint64_t last_applied_{0};

    // 1-based Raft log (index 0 is a dummy sentinel entry)
    std::vector<RaftLogEntry> log_;
    std::vector<RaftPeer> peers_;
    std::unordered_map<uint32_t, uint64_t> next_index_;
    std::unordered_map<uint32_t, uint64_t> match_index_;

    std::chrono::steady_clock::time_point last_heartbeat_;
    std::chrono::milliseconds election_timeout_{200};

    ApplyCallback apply_cb_;
    InProcessClusterBus* bus_{nullptr};

    struct UdpTransport;
    std::unique_ptr<UdpTransport> udp_;

    std::unique_ptr<zephyr::cpp26::execution::thread_pool_scheduler> sched_;
};

} // namespace zephyr::net

#endif // ZEPHYR_NET_RAFT_HPP
