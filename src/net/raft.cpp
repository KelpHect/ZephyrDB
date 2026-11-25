#include "zephyr/net/raft.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>

#if defined(__EMSCRIPTEN__)
  // Emscripten: UDP sockets stubbed; in-process bus remains functional.
#elif defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace zephyr::net {

namespace {

constexpr uint32_t kRaftMagic = 0x5A524654u; // "ZRFT"
constexpr uint8_t kMsgRvReq = 1;
constexpr uint8_t kMsgRvRep = 2;
constexpr uint8_t kMsgAeReq = 3;
constexpr uint8_t kMsgAeRep = 4;

#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

struct WsaGuard {
    WsaGuard() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WsaGuard() { WSACleanup(); }
};

void ensure_wsa_init() {
    static WsaGuard guard;
}

void close_socket(SocketHandle s) {
    if (s != kInvalidSocket) {
        closesocket(s);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void ensure_wsa_init() {}

void close_socket(SocketHandle s) {
    if (s != kInvalidSocket) {
        ::close(s);
    }
}
#endif
#endif

void write_u8(std::vector<uint8_t>& buf, uint8_t v) {
    buf.push_back(v);
}

void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void write_str(std::vector<uint8_t>& buf, std::string_view sv) {
    write_u32(buf, static_cast<uint32_t>(sv.size()));
    const auto* p = reinterpret_cast<const uint8_t*>(sv.data());
    buf.insert(buf.end(), p, p + sv.size());
}

bool read_u8(const uint8_t* data, size_t len, size_t& off, uint8_t& out) {
    if (off + 1 > len) return false;
    out = data[off++];
    return true;
}

bool read_u32(const uint8_t* data, size_t len, size_t& off, uint32_t& out) {
    if (off + 4 > len) return false;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(data[off + i]) << (i * 8);
    }
    off += 4;
    out = v;
    return true;
}

bool read_u64(const uint8_t* data, size_t len, size_t& off, uint64_t& out) {
    if (off + 8 > len) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(data[off + i]) << (i * 8);
    }
    off += 8;
    out = v;
    return true;
}

bool read_str(const uint8_t* data, size_t len, size_t& off, std::string& out) {
    uint32_t slen = 0;
    if (!read_u32(data, len, off, slen)) return false;
    if (off + slen > len) return false;
    out.assign(reinterpret_cast<const char*>(data + off), slen);
    off += slen;
    return true;
}

std::vector<uint8_t> encode_header(uint8_t type, uint64_t corr_id) {
    std::vector<uint8_t> buf;
    buf.reserve(64);
    write_u32(buf, kRaftMagic);
    write_u8(buf, type);
    write_u64(buf, corr_id);
    return buf;
}

std::vector<uint8_t> encode_rv_req(uint64_t corr_id, const RequestVoteArgs& a) {
    auto buf = encode_header(kMsgRvReq, corr_id);
    write_u64(buf, a.term);
    write_u32(buf, a.candidate_id);
    write_u64(buf, a.last_log_index);
    write_u64(buf, a.last_log_term);
    return buf;
}

bool decode_rv_req(const uint8_t* d, size_t n, size_t& off, RequestVoteArgs& a) {
    return read_u64(d, n, off, a.term) &&
           read_u32(d, n, off, a.candidate_id) &&
           read_u64(d, n, off, a.last_log_index) &&
           read_u64(d, n, off, a.last_log_term);
}

std::vector<uint8_t> encode_rv_rep(uint64_t corr_id, const RequestVoteReply& r) {
    auto buf = encode_header(kMsgRvRep, corr_id);
    write_u64(buf, r.term);
    write_u8(buf, r.vote_granted ? 1 : 0);
    return buf;
}

bool decode_rv_rep(const uint8_t* d, size_t n, size_t& off, RequestVoteReply& r) {
    uint8_t granted = 0;
    if (!read_u64(d, n, off, r.term) || !read_u8(d, n, off, granted)) return false;
    r.vote_granted = (granted != 0);
    return true;
}

std::vector<uint8_t> encode_ae_req(uint64_t corr_id, const AppendEntriesArgs& a) {
    auto buf = encode_header(kMsgAeReq, corr_id);
    write_u64(buf, a.term);
    write_u32(buf, a.leader_id);
    write_u64(buf, a.prev_log_index);
    write_u64(buf, a.prev_log_term);
    write_u64(buf, a.leader_commit);
    write_u32(buf, static_cast<uint32_t>(a.entries.size()));
    for (const auto& e : a.entries) {
        write_u64(buf, e.term);
        write_u64(buf, e.lsn);
        write_str(buf, e.sql);
    }
    return buf;
}

bool decode_ae_req(const uint8_t* d, size_t n, size_t& off, AppendEntriesArgs& a) {
    uint32_t count = 0;
    if (!read_u64(d, n, off, a.term) ||
        !read_u32(d, n, off, a.leader_id) ||
        !read_u64(d, n, off, a.prev_log_index) ||
        !read_u64(d, n, off, a.prev_log_term) ||
        !read_u64(d, n, off, a.leader_commit) ||
        !read_u32(d, n, off, count)) {
        return false;
    }
    a.entries.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!read_u64(d, n, off, a.entries[i].term) ||
            !read_u64(d, n, off, a.entries[i].lsn) ||
            !read_str(d, n, off, a.entries[i].sql)) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> encode_ae_rep(uint64_t corr_id, const AppendEntriesReply& r) {
    auto buf = encode_header(kMsgAeRep, corr_id);
    write_u64(buf, r.term);
    write_u64(buf, r.match_index);
    write_u8(buf, r.success ? 1 : 0);
    return buf;
}

bool decode_ae_rep(const uint8_t* d, size_t n, size_t& off, AppendEntriesReply& r) {
    uint8_t ok = 0;
    if (!read_u64(d, n, off, r.term) ||
        !read_u64(d, n, off, r.match_index) ||
        !read_u8(d, n, off, ok)) {
        return false;
    }
    r.success = (ok != 0);
    return true;
}

} // namespace

// ============================================================================
// InProcessClusterBus
// ============================================================================

void InProcessClusterBus::register_node(uint32_t node_id, RaftNode* node) {
    std::lock_guard<std::mutex> lk(mu_);
    nodes_[node_id] = node;
}

void InProcessClusterBus::unregister_node(uint32_t node_id) {
    std::lock_guard<std::mutex> lk(mu_);
    nodes_.erase(node_id);
}

void InProcessClusterBus::set_partition(uint32_t a, uint32_t b, bool partitioned) {
    std::lock_guard<std::mutex> lk(mu_);
    uint64_t key = link_key(a, b);
    if (partitioned) {
        partitioned_links_[key] = true;
    } else {
        partitioned_links_.erase(key);
    }
}

bool InProcessClusterBus::send_request_vote(uint32_t from_id, uint32_t to_id,
                                            const RequestVoteArgs& args, RequestVoteReply& reply) {
    RaftNode* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (partitioned_links_.count(link_key(from_id, to_id)) > 0) return false;
        auto it = nodes_.find(to_id);
        if (it == nodes_.end() || !it->second || !it->second->is_running()) return false;
        target = it->second;
    }
    reply = target->handle_request_vote(args);
    return true;
}

bool InProcessClusterBus::send_append_entries(uint32_t from_id, uint32_t to_id,
                                              const AppendEntriesArgs& args, AppendEntriesReply& reply) {
    RaftNode* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (partitioned_links_.count(link_key(from_id, to_id)) > 0) return false;
        auto it = nodes_.find(to_id);
        if (it == nodes_.end() || !it->second || !it->second->is_running()) return false;
        target = it->second;
    }
    reply = target->handle_append_entries(args);
    return true;
}

// ============================================================================
// UdpTransport
// ============================================================================

struct RaftNode::UdpTransport {
#if !defined(__EMSCRIPTEN__)
    SocketHandle sock = kInvalidSocket;
#endif
    std::mutex send_mu;
    std::atomic<uint64_t> next_corr{1};

    struct Pending {
        bool ready = false;
        std::vector<uint8_t> payload;
    };
    std::mutex pending_mu;
    std::condition_variable pending_cv;
    std::unordered_map<uint64_t, Pending> pending;

    ~UdpTransport() {
        close();
    }

    bool open(uint16_t port, uint16_t& out_bound_port) {
#if defined(__EMSCRIPTEN__)
        (void)port;
        (void)out_bound_port;
        return false;
#else
        ensure_wsa_init();
        sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kInvalidSocket) return false;

        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }

        sockaddr_in bound{};
#if defined(_WIN32)
        int blen = sizeof(bound);
#else
        socklen_t blen = sizeof(bound);
#endif
        if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
            out_bound_port = ntohs(bound.sin_port);
        } else {
            out_bound_port = port;
        }
        return true;
#endif
    }

    void close() {
#if !defined(__EMSCRIPTEN__)
        if (sock != kInvalidSocket) {
            close_socket(sock);
            sock = kInvalidSocket;
        }
#endif
        {
            std::lock_guard<std::mutex> lk(pending_mu);
            pending.clear();
        }
        pending_cv.notify_all();
    }

    bool send_raw(const std::string& host, uint16_t port, const std::vector<uint8_t>& data) {
#if defined(__EMSCRIPTEN__)
        (void)host;
        (void)port;
        (void)data;
        return false;
#else
        if (sock == kInvalidSocket) return false;
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) {
            return false;
        }
        std::lock_guard<std::mutex> lk(send_mu);
        int sent = ::sendto(sock, reinterpret_cast<const char*>(data.data()),
                            static_cast<int>(data.size()), 0,
                            reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
        return sent == static_cast<int>(data.size());
#endif
    }

    bool send_and_wait(const std::string& host, uint16_t port,
                       uint64_t corr_id, const std::vector<uint8_t>& req,
                       std::vector<uint8_t>& out_rep) {
        {
            std::lock_guard<std::mutex> lk(pending_mu);
            pending[corr_id] = Pending{};
        }
        if (!send_raw(host, port, req)) {
            std::lock_guard<std::mutex> lk(pending_mu);
            pending.erase(corr_id);
            return false;
        }
        std::unique_lock<std::mutex> lk(pending_mu);
        bool ok = pending_cv.wait_for(lk, std::chrono::milliseconds(45), [&] {
            auto it = pending.find(corr_id);
            return it == pending.end() || it->second.ready;
        });
        auto it = pending.find(corr_id);
        if (ok && it != pending.end() && it->second.ready) {
            out_rep = std::move(it->second.payload);
            pending.erase(it);
            return true;
        }
        if (it != pending.end()) {
            pending.erase(it);
        }
        return false;
    }
};

// ============================================================================
// RaftNode
// ============================================================================

RaftNode::RaftNode() {
    log_.push_back(RaftLogEntry{0, 0, ""}); // 1-based sentinel at index 0
}

RaftNode::~RaftNode() {
    stop();
}

std::vector<RaftPeer> RaftNode::parse_peers(std::string_view peers_csv, uint32_t self_id) {
    std::vector<RaftPeer> result;
    if (peers_csv.empty()) return result;

    uint32_t auto_id = 1;
    size_t start = 0;
    while (start <= peers_csv.size()) {
        size_t comma = peers_csv.find(',', start);
        std::string_view tok = (comma == std::string_view::npos)
                                   ? peers_csv.substr(start)
                                   : peers_csv.substr(start, comma - start);
        // Trim whitespace
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.remove_suffix(1);

        if (!tok.empty()) {
            RaftPeer peer;
            size_t at = tok.find('@');
            if (at != std::string_view::npos) {
                peer.id = static_cast<uint32_t>(std::stoul(std::string(tok.substr(0, at))));
                tok.remove_prefix(at + 1);
            } else {
                if (auto_id == self_id) ++auto_id;
                peer.id = auto_id++;
            }
            size_t colon = tok.rfind(':');
            if (colon != std::string_view::npos) {
                peer.host = std::string(tok.substr(0, colon));
                peer.port = static_cast<uint16_t>(std::stoul(std::string(tok.substr(colon + 1))));
            } else {
                peer.host = std::string(tok);
                peer.port = 7000;
            }
            if (peer.id != self_id && peer.port > 0) {
                result.push_back(std::move(peer));
            }
        }

        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return result;
}

bool RaftNode::start(uint32_t node_id, uint16_t port, std::string_view peers_csv, ApplyCallback apply_cb) {
    stop();

#if defined(__EMSCRIPTEN__)
    (void)node_id;
    (void)port;
    (void)peers_csv;
    (void)apply_cb;
    return false;
#else
    auto udp = std::make_unique<UdpTransport>();
    uint16_t bport = 0;
    if (!udp->open(port, bport)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        node_id_.store(node_id, std::memory_order_release);
        bound_port_.store(bport, std::memory_order_release);
        state_ = RaftState::Follower;
        leader_id_ = 0;
        current_term_ = 0;
        voted_for_ = 0;
        commit_index_ = 0;
        last_applied_ = 0;
        log_.assign(1, RaftLogEntry{0, 0, ""});
        peers_ = parse_peers(peers_csv, node_id);
        next_index_.clear();
        match_index_.clear();
        apply_cb_ = std::move(apply_cb);
        bus_ = nullptr;
        udp_ = std::move(udp);
        election_timeout_ = random_election_timeout();
        last_heartbeat_ = std::chrono::steady_clock::now();

        state_atomic_.store(state_, std::memory_order_release);
        leader_id_atomic_.store(leader_id_, std::memory_order_release);
        term_atomic_.store(current_term_, std::memory_order_release);
        commit_atomic_.store(commit_index_, std::memory_order_release);
    }

    running_.store(true, std::memory_order_release);
    sched_ = std::make_unique<zephyr::cpp26::execution::thread_pool_scheduler>(2);

    zephyr::cpp26::execution::start_detached(
        zephyr::cpp26::execution::schedule_task(*sched_, [this] { udp_recv_loop(); }));
    zephyr::cpp26::execution::start_detached(
        zephyr::cpp26::execution::schedule_task(*sched_, [this] { timer_loop(); }));
    return true;
#endif
}

bool RaftNode::start_in_process(uint32_t node_id, InProcessClusterBus& bus,
                                const std::vector<uint32_t>& peer_ids, ApplyCallback apply_cb) {
    stop();

    {
        std::lock_guard<std::mutex> lk(mu_);
        node_id_.store(node_id, std::memory_order_release);
        bound_port_.store(0, std::memory_order_release);
        state_ = RaftState::Follower;
        leader_id_ = 0;
        current_term_ = 0;
        voted_for_ = 0;
        commit_index_ = 0;
        last_applied_ = 0;
        log_.assign(1, RaftLogEntry{0, 0, ""});
        peers_.clear();
        for (uint32_t pid : peer_ids) {
            if (pid != node_id) {
                peers_.push_back(RaftPeer{pid, "", 0});
            }
        }
        next_index_.clear();
        match_index_.clear();
        apply_cb_ = std::move(apply_cb);
        bus_ = &bus;
        udp_.reset();
        election_timeout_ = random_election_timeout();
        last_heartbeat_ = std::chrono::steady_clock::now();

        state_atomic_.store(state_, std::memory_order_release);
        leader_id_atomic_.store(leader_id_, std::memory_order_release);
        term_atomic_.store(current_term_, std::memory_order_release);
        commit_atomic_.store(commit_index_, std::memory_order_release);
    }

    bus.register_node(node_id, this);
    running_.store(true, std::memory_order_release);
    sched_ = std::make_unique<zephyr::cpp26::execution::thread_pool_scheduler>(1);
    zephyr::cpp26::execution::start_detached(
        zephyr::cpp26::execution::schedule_task(*sched_, [this] { timer_loop(); }));
    return true;
}

void RaftNode::stop() {
    bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running && !sched_ && !udp_ && !bus_) {
        return;
    }

    if (bus_) {
        bus_->unregister_node(node_id_.load(std::memory_order_acquire));
        bus_ = nullptr;
    }
    if (udp_) {
        udp_->close();
    }
    if (sched_) {
        sched_->stop();
        sched_.reset();
    }
    udp_.reset();
    bound_port_.store(0, std::memory_order_release);
}

RaftState RaftNode::state() const {
    return state_atomic_.load(std::memory_order_acquire);
}

uint32_t RaftNode::leader_id() const {
    return leader_id_atomic_.load(std::memory_order_acquire);
}

uint64_t RaftNode::current_term() const {
    return term_atomic_.load(std::memory_order_acquire);
}

uint64_t RaftNode::commit_index() const {
    return commit_atomic_.load(std::memory_order_acquire);
}

uint64_t RaftNode::last_log_index_locked() const noexcept {
    return log_.empty() ? 0 : static_cast<uint64_t>(log_.size() - 1);
}

uint64_t RaftNode::last_log_term_locked() const noexcept {
    return log_.empty() ? 0 : log_.back().term;
}

std::chrono::milliseconds RaftNode::random_election_timeout() {
    thread_local std::mt19937 rng{static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() ^
        std::hash<std::thread::id>{}(std::this_thread::get_id()))};
    std::uniform_int_distribution<int> dist(150, 300);
    return std::chrono::milliseconds(dist(rng));
}

bool RaftNode::propose_wal_entry(uint64_t lsn, std::string_view sql) {
    std::vector<RaftLogEntry> to_apply;
    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!running_.load(std::memory_order_acquire) || state_ != RaftState::Leader) {
            return false;
        }
        log_.push_back(RaftLogEntry{current_term_, lsn, std::string(sql)});
        if (peers_.empty()) {
            advance_commit_locked(to_apply);
        } else {
            send_heartbeats_locked(lk);
            // Note: send_heartbeats_locked may release and re-acquire lk and advance commit
        }
    }
    invoke_apply(to_apply);
    return true;
}

RequestVoteReply RaftNode::handle_request_vote(const RequestVoteArgs& args) {
    std::lock_guard<std::mutex> lk(mu_);
    RequestVoteReply reply{current_term_, false};

    if (args.term < current_term_) {
        return reply;
    }

    if (args.term > current_term_) {
        current_term_ = args.term;
        state_ = RaftState::Follower;
        voted_for_ = 0;
        leader_id_ = 0;
        state_atomic_.store(state_, std::memory_order_release);
        term_atomic_.store(current_term_, std::memory_order_release);
        leader_id_atomic_.store(leader_id_, std::memory_order_release);
    }

    reply.term = current_term_;

    const uint64_t my_last_term = last_log_term_locked();
    const uint64_t my_last_idx = last_log_index_locked();
    const bool log_ok = (args.last_log_term > my_last_term) ||
                        (args.last_log_term == my_last_term && args.last_log_index >= my_last_idx);

    if ((voted_for_ == 0 || voted_for_ == args.candidate_id) && log_ok) {
        voted_for_ = args.candidate_id;
        last_heartbeat_ = std::chrono::steady_clock::now();
        reply.vote_granted = true;
    }
    return reply;
}

AppendEntriesReply RaftNode::handle_append_entries(const AppendEntriesArgs& args) {
    std::vector<RaftLogEntry> to_apply;
    AppendEntriesReply reply{0, 0, false};

    {
        std::lock_guard<std::mutex> lk(mu_);
        reply.term = current_term_;

        if (args.term < current_term_) {
            return reply;
        }

        if (args.term > current_term_ || state_ != RaftState::Follower) {
            current_term_ = args.term;
            state_ = RaftState::Follower;
            voted_for_ = 0;
            state_atomic_.store(state_, std::memory_order_release);
            term_atomic_.store(current_term_, std::memory_order_release);
        }

        leader_id_ = args.leader_id;
        leader_id_atomic_.store(leader_id_, std::memory_order_release);
        last_heartbeat_ = std::chrono::steady_clock::now();
        reply.term = current_term_;

        if (args.prev_log_index > last_log_index_locked()) {
            reply.match_index = last_log_index_locked();
            return reply;
        }
        if (log_[args.prev_log_index].term != args.prev_log_term) {
            reply.match_index = args.prev_log_index > 0 ? args.prev_log_index - 1 : 0;
            return reply;
        }

        uint64_t idx = args.prev_log_index + 1;
        for (const auto& entry : args.entries) {
            if (idx < log_.size()) {
                if (log_[idx].term != entry.term) {
                    log_.resize(idx);
                    log_.push_back(entry);
                }
            } else {
                log_.push_back(entry);
            }
            ++idx;
        }

        if (args.leader_commit > commit_index_) {
            uint64_t new_commit = std::min(args.leader_commit, last_log_index_locked());
            if (new_commit > commit_index_) {
                commit_index_ = new_commit;
                commit_atomic_.store(commit_index_, std::memory_order_release);
                while (last_applied_ < commit_index_) {
                    ++last_applied_;
                    to_apply.push_back(log_[last_applied_]);
                }
            }
        }

        reply.success = true;
        reply.match_index = last_log_index_locked();
    }

    invoke_apply(to_apply);
    return reply;
}

void RaftNode::advance_commit_locked(std::vector<RaftLogEntry>& to_apply) {
    const uint64_t last_idx = last_log_index_locked();
    const size_t total_nodes = peers_.size() + 1;
    const size_t quorum = total_nodes / 2 + 1;

    for (uint64_t n = last_idx; n > commit_index_; --n) {
        if (log_[n].term != current_term_) continue;
        size_t replicated = 1; // self
        for (const auto& peer : peers_) {
            auto it = match_index_.find(peer.id);
            if (it != match_index_.end() && it->second >= n) {
                ++replicated;
            }
        }
        if (replicated >= quorum) {
            commit_index_ = n;
            commit_atomic_.store(commit_index_, std::memory_order_release);
            break;
        }
    }

    while (last_applied_ < commit_index_) {
        ++last_applied_;
        to_apply.push_back(log_[last_applied_]);
    }
}

void RaftNode::invoke_apply(const std::vector<RaftLogEntry>& entries) {
    if (!apply_cb_ || entries.empty()) return;
    for (const auto& e : entries) {
        apply_cb_(e.lsn, e.sql);
    }
}

bool RaftNode::rpc_request_vote(const RaftPeer& peer, const RequestVoteArgs& args, RequestVoteReply& reply) {
    if (bus_) {
        return bus_->send_request_vote(node_id_.load(std::memory_order_acquire), peer.id, args, reply);
    }
    if (!udp_) return false;
    uint64_t corr = udp_->next_corr.fetch_add(1, std::memory_order_relaxed);
    auto req_bytes = encode_rv_req(corr, args);
    std::vector<uint8_t> rep_bytes;
    if (!udp_->send_and_wait(peer.host, peer.port, corr, req_bytes, rep_bytes)) {
        return false;
    }
    size_t off = 0;
    return decode_rv_rep(rep_bytes.data(), rep_bytes.size(), off, reply);
}

bool RaftNode::rpc_append_entries(const RaftPeer& peer, const AppendEntriesArgs& args, AppendEntriesReply& reply) {
    if (bus_) {
        return bus_->send_append_entries(node_id_.load(std::memory_order_acquire), peer.id, args, reply);
    }
    if (!udp_) return false;
    uint64_t corr = udp_->next_corr.fetch_add(1, std::memory_order_relaxed);
    auto req_bytes = encode_ae_req(corr, args);
    std::vector<uint8_t> rep_bytes;
    if (!udp_->send_and_wait(peer.host, peer.port, corr, req_bytes, rep_bytes)) {
        return false;
    }
    size_t off = 0;
    return decode_ae_rep(rep_bytes.data(), rep_bytes.size(), off, reply);
}

void RaftNode::start_election_locked(std::unique_lock<std::mutex>& lk) {
    state_ = RaftState::Candidate;
    ++current_term_;
    const uint32_t self_id = node_id_.load(std::memory_order_acquire);
    voted_for_ = self_id;
    leader_id_ = 0;
    last_heartbeat_ = std::chrono::steady_clock::now();
    election_timeout_ = random_election_timeout();

    state_atomic_.store(state_, std::memory_order_release);
    term_atomic_.store(current_term_, std::memory_order_release);
    leader_id_atomic_.store(leader_id_, std::memory_order_release);

    const uint64_t election_term = current_term_;
    const RequestVoteArgs args{
        election_term,
        self_id,
        last_log_index_locked(),
        last_log_term_locked()
    };
    const auto peers_copy = peers_;
    const size_t quorum = (peers_copy.size() + 1) / 2 + 1;

    size_t votes = 1; // voted for self
    if (votes >= quorum) {
        state_ = RaftState::Leader;
        leader_id_ = self_id;
        state_atomic_.store(state_, std::memory_order_release);
        leader_id_atomic_.store(leader_id_, std::memory_order_release);
        const uint64_t next_idx = last_log_index_locked() + 1;
        for (const auto& p : peers_copy) {
            next_index_[p.id] = next_idx;
            match_index_[p.id] = 0;
        }
        return;
    }

    lk.unlock();

    for (const auto& peer : peers_copy) {
        if (!running_.load(std::memory_order_acquire)) break;
        RequestVoteReply rep{};
        if (rpc_request_vote(peer, args, rep)) {
            std::lock_guard<std::mutex> rlk(mu_);
            if (rep.term > current_term_) {
                current_term_ = rep.term;
                state_ = RaftState::Follower;
                voted_for_ = 0;
                leader_id_ = 0;
                state_atomic_.store(state_, std::memory_order_release);
                term_atomic_.store(current_term_, std::memory_order_release);
                leader_id_atomic_.store(leader_id_, std::memory_order_release);
                lk.lock();
                return;
            }
            if (state_ == RaftState::Candidate && current_term_ == election_term && rep.vote_granted) {
                ++votes;
                if (votes >= quorum) {
                    state_ = RaftState::Leader;
                    leader_id_ = self_id;
                    state_atomic_.store(state_, std::memory_order_release);
                    leader_id_atomic_.store(leader_id_, std::memory_order_release);
                    const uint64_t next_idx = last_log_index_locked() + 1;
                    for (const auto& p : peers_) {
                        next_index_[p.id] = next_idx;
                        match_index_[p.id] = 0;
                    }
                    break;
                }
            }
        }
    }

    lk.lock();
}

void RaftNode::send_heartbeats_locked(std::unique_lock<std::mutex>& lk) {
    if (state_ != RaftState::Leader) return;
    const uint64_t term = current_term_;
    const uint32_t self_id = node_id_.load(std::memory_order_acquire);
    const uint64_t commit = commit_index_;
    const auto peers_copy = peers_;

    struct PeerReq {
        RaftPeer peer;
        AppendEntriesArgs args;
    };
    std::vector<PeerReq> reqs;
    reqs.reserve(peers_copy.size());

    for (const auto& peer : peers_copy) {
        uint64_t nxt = next_index_.count(peer.id) ? next_index_[peer.id] : (last_log_index_locked() + 1);
        if (nxt == 0) nxt = 1;
        uint64_t prev_idx = nxt - 1;
        if (prev_idx > last_log_index_locked()) prev_idx = last_log_index_locked();

        AppendEntriesArgs a;
        a.term = term;
        a.leader_id = self_id;
        a.prev_log_index = prev_idx;
        a.prev_log_term = log_[prev_idx].term;
        a.leader_commit = commit;
        for (uint64_t i = prev_idx + 1; i < log_.size(); ++i) {
            a.entries.push_back(log_[i]);
        }
        reqs.push_back(PeerReq{peer, std::move(a)});
    }

    lk.unlock();

    std::vector<std::pair<uint32_t, AppendEntriesReply>> replies;
    replies.reserve(reqs.size());
    for (const auto& pr : reqs) {
        if (!running_.load(std::memory_order_acquire)) break;
        AppendEntriesReply rep{};
        if (rpc_append_entries(pr.peer, pr.args, rep)) {
            replies.emplace_back(pr.peer.id, rep);
        }
    }

    std::vector<RaftLogEntry> to_apply;
    lk.lock();

    for (const auto& [pid, rep] : replies) {
        if (rep.term > current_term_) {
            current_term_ = rep.term;
            state_ = RaftState::Follower;
            voted_for_ = 0;
            leader_id_ = 0;
            state_atomic_.store(state_, std::memory_order_release);
            term_atomic_.store(current_term_, std::memory_order_release);
            leader_id_atomic_.store(leader_id_, std::memory_order_release);
            return;
        }
        if (state_ != RaftState::Leader || current_term_ != term) continue;

        if (rep.success) {
            match_index_[pid] = std::max(match_index_[pid], rep.match_index);
            next_index_[pid] = match_index_[pid] + 1;
        } else {
            uint64_t& nxt = next_index_[pid];
            if (nxt > 1) --nxt;
        }
    }

    if (state_ == RaftState::Leader && current_term_ == term) {
        advance_commit_locked(to_apply);
    }

    lk.unlock();
    invoke_apply(to_apply);
    lk.lock();
}

void RaftNode::timer_loop() {
    auto last_hb_sent = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!running_.load(std::memory_order_acquire)) break;

            auto now = std::chrono::steady_clock::now();
            if (state_ == RaftState::Leader) {
                if (now - last_hb_sent >= std::chrono::milliseconds(40)) {
                    last_hb_sent = now;
                    send_heartbeats_locked(lk);
                }
            } else {
                if (now - last_heartbeat_ >= election_timeout_) {
                    start_election_locked(lk);
                    if (state_ == RaftState::Leader) {
                        last_hb_sent = std::chrono::steady_clock::now();
                        send_heartbeats_locked(lk);
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
}

void RaftNode::udp_recv_loop() {
#if !defined(__EMSCRIPTEN__)
    std::vector<uint8_t> buf(65536);
    while (running_.load(std::memory_order_acquire) && udp_ && udp_->sock != kInvalidSocket) {
        sockaddr_in sender{};
#if defined(_WIN32)
        int slen = sizeof(sender);
#else
        socklen_t slen = sizeof(sender);
#endif
        int n = ::recvfrom(udp_->sock, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&sender), &slen);
        if (n <= 0) {
            if (!running_.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const size_t len = static_cast<size_t>(n);
        size_t off = 0;
        uint32_t magic = 0;
        uint8_t msg_type = 0;
        uint64_t corr_id = 0;
        if (!read_u32(buf.data(), len, off, magic) || magic != kRaftMagic ||
            !read_u8(buf.data(), len, off, msg_type) ||
            !read_u64(buf.data(), len, off, corr_id)) {
            continue;
        }

        if (msg_type == kMsgRvReq) {
            RequestVoteArgs args{};
            if (decode_rv_req(buf.data(), len, off, args)) {
                auto rep = handle_request_vote(args);
                auto pkt = encode_rv_rep(corr_id, rep);
                std::lock_guard<std::mutex> lk(udp_->send_mu);
                ::sendto(udp_->sock, reinterpret_cast<const char*>(pkt.data()),
                         static_cast<int>(pkt.size()), 0,
                         reinterpret_cast<const sockaddr*>(&sender), slen);
            }
        } else if (msg_type == kMsgAeReq) {
            AppendEntriesArgs args{};
            if (decode_ae_req(buf.data(), len, off, args)) {
                auto rep = handle_append_entries(args);
                auto pkt = encode_ae_rep(corr_id, rep);
                std::lock_guard<std::mutex> lk(udp_->send_mu);
                ::sendto(udp_->sock, reinterpret_cast<const char*>(pkt.data()),
                         static_cast<int>(pkt.size()), 0,
                         reinterpret_cast<const sockaddr*>(&sender), slen);
            }
        } else if (msg_type == kMsgRvRep || msg_type == kMsgAeRep) {
            std::lock_guard<std::mutex> lk(udp_->pending_mu);
            auto it = udp_->pending.find(corr_id);
            if (it != udp_->pending.end()) {
                it->second.payload.assign(buf.begin() + static_cast<std::ptrdiff_t>(off),
                                          buf.begin() + static_cast<std::ptrdiff_t>(len));
                it->second.ready = true;
                udp_->pending_cv.notify_all();
            }
        }
    }
#endif
}

} // namespace zephyr::net
