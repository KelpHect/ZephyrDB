#include "zephyr/net/pgwire.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__EMSCRIPTEN__)
  // Emscripten: TCP server stubbed cleanly.
#elif defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace zephyr::net {

namespace {

#if !defined(__EMSCRIPTEN__)
#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

struct PgWsaGuard {
    PgWsaGuard() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~PgWsaGuard() { WSACleanup(); }
};

void ensure_wsa() {
    static PgWsaGuard guard;
}

void close_sock(uintptr_t raw) {
    auto s = static_cast<SocketHandle>(raw);
    if (s != kInvalidSocket) {
        closesocket(s);
    }
}
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void ensure_wsa() {}

void close_sock(uintptr_t raw) {
    auto s = static_cast<SocketHandle>(raw);
    if (s != kInvalidSocket) {
        ::shutdown(s, SHUT_RDWR);
        ::close(s);
    }
}
#endif

bool recv_exact(SocketHandle s, void* dst, size_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < n) {
        int r = ::recv(s, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool send_all(SocketHandle s, const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while (sent < n) {
        int r = ::send(s, reinterpret_cast<const char*>(p + sent), static_cast<int>(n - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

void append_be16(std::vector<uint8_t>& buf, int16_t v) {
    uint16_t u = static_cast<uint16_t>(v);
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(u & 0xFF));
}

void append_be32(std::vector<uint8_t>& buf, int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(u & 0xFF));
}

int32_t read_be32(const uint8_t* p) {
    uint32_t u = (static_cast<uint32_t>(p[0]) << 24) |
                 (static_cast<uint32_t>(p[1]) << 16) |
                 (static_cast<uint32_t>(p[2]) << 8) |
                 static_cast<uint32_t>(p[3]);
    return static_cast<int32_t>(u);
}

void append_cstr(std::vector<uint8_t>& buf, std::string_view sv) {
    buf.insert(buf.end(), sv.begin(), sv.end());
    buf.push_back(0);
}

void append_msg_header(std::vector<uint8_t>& buf, char type, size_t body_len) {
    buf.push_back(static_cast<uint8_t>(type));
    append_be32(buf, static_cast<int32_t>(4 + body_len));
}

void send_auth_ok(SocketHandle s) {
    std::vector<uint8_t> out;
    append_msg_header(out, 'R', 4);
    append_be32(out, 0);
    send_all(s, out.data(), out.size());
}

void send_param_status(SocketHandle s, std::string_view k, std::string_view v) {
    std::vector<uint8_t> out;
    append_msg_header(out, 'S', k.size() + 1 + v.size() + 1);
    append_cstr(out, k);
    append_cstr(out, v);
    send_all(s, out.data(), out.size());
}

void send_backend_key(SocketHandle s, int32_t pid, int32_t secret) {
    std::vector<uint8_t> out;
    append_msg_header(out, 'K', 8);
    append_be32(out, pid);
    append_be32(out, secret);
    send_all(s, out.data(), out.size());
}

void send_ready_for_query(SocketHandle s, char status = 'I') {
    std::vector<uint8_t> out;
    append_msg_header(out, 'Z', 1);
    out.push_back(static_cast<uint8_t>(status));
    send_all(s, out.data(), out.size());
}

void send_error_response(SocketHandle s, std::string_view msg) {
    std::vector<uint8_t> body;
    body.push_back('S');
    append_cstr(body, "ERROR");
    body.push_back('C');
    append_cstr(body, "42000");
    body.push_back('M');
    append_cstr(body, msg);
    body.push_back(0);

    std::vector<uint8_t> out;
    append_msg_header(out, 'E', body.size());
    out.insert(out.end(), body.begin(), body.end());
    send_all(s, out.data(), out.size());
}

void send_command_complete(SocketHandle s, std::string_view tag) {
    std::vector<uint8_t> out;
    append_msg_header(out, 'C', tag.size() + 1);
    append_cstr(out, tag);
    send_all(s, out.data(), out.size());
}

void send_empty_query(SocketHandle s) {
    std::vector<uint8_t> out;
    append_msg_header(out, 'I', 0);
    send_all(s, out.data(), out.size());
}

std::pair<int32_t, int16_t> pg_type_for_zephyr(int32_t ztype) {
    switch (ztype) {
        case ZEPHYR_TYPE_I64:   return {20, int16_t{8}};   // INT8
        case ZEPHYR_TYPE_F64:   return {701, int16_t{8}};  // FLOAT8
        case ZEPHYR_TYPE_BOOL:  return {16, int16_t{1}};   // BOOL
        case ZEPHYR_TYPE_BYTEA: return {17, int16_t{-1}};  // BYTEA
        case ZEPHYR_TYPE_TEXT:
        default:                return {25, int16_t{-1}};  // TEXT
    }
}

void send_row_description(SocketHandle s, const ZephyrResultSet* rs) {
    std::vector<uint8_t> body;
    const int16_t ncols = static_cast<int16_t>(rs->col_count);
    append_be16(body, ncols);
    for (uint64_t c = 0; c < rs->col_count; ++c) {
        const ZephyrColumnDesc& col = rs->columns[c];
        std::string_view name(col.name ? col.name : "", col.name_len);
        append_cstr(body, name);
        append_be32(body, 0);     // table OID
        append_be16(body, 0);     // column attr number
        auto [oid, typlen] = pg_type_for_zephyr(col.type);
        append_be32(body, oid);
        append_be16(body, typlen);
        append_be32(body, -1);    // type modifier
        append_be16(body, 0);     // text format
    }

    std::vector<uint8_t> out;
    append_msg_header(out, 'T', body.size());
    out.insert(out.end(), body.begin(), body.end());
    send_all(s, out.data(), out.size());
}

std::string format_value_text(const ZephyrValue& v) {
    switch (v.type) {
        case ZEPHYR_TYPE_I64:
            return std::to_string(v.as.i64);
        case ZEPHYR_TYPE_F64: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.15g", v.as.f64);
            return std::string(buf);
        }
        case ZEPHYR_TYPE_BOOL:
            return v.as.boolean ? "t" : "f";
        case ZEPHYR_TYPE_TEXT:
            if (v.as.slice.ptr && v.as.slice.len > 0) {
                return std::string(reinterpret_cast<const char*>(v.as.slice.ptr), v.as.slice.len);
            }
            return {};
        case ZEPHYR_TYPE_BYTEA: {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string out = "\\x";
            out.reserve(2 + v.as.slice.len * 2);
            for (uint32_t i = 0; i < v.as.slice.len; ++i) {
                uint8_t b = v.as.slice.ptr[i];
                out.push_back(kHex[(b >> 4) & 0x0F]);
                out.push_back(kHex[b & 0x0F]);
            }
            return out;
        }
        default:
            return {};
    }
}

void send_data_rows(SocketHandle s, const ZephyrResultSet* rs) {
    const int16_t ncols = static_cast<int16_t>(rs->col_count);
    for (uint64_t r = 0; r < rs->row_count; ++r) {
        std::vector<uint8_t> body;
        append_be16(body, ncols);
        for (uint64_t c = 0; c < rs->col_count; ++c) {
            const ZephyrValue& val = rs->values[r * rs->col_count + c];
            if (val.type == ZEPHYR_TYPE_NULL) {
                append_be32(body, -1);
            } else {
                std::string text = format_value_text(val);
                append_be32(body, static_cast<int32_t>(text.size()));
                body.insert(body.end(), text.begin(), text.end());
            }
        }
        std::vector<uint8_t> out;
        append_msg_header(out, 'D', body.size());
        out.insert(out.end(), body.begin(), body.end());
        if (!send_all(s, out.data(), out.size())) return;
    }
}

std::string build_command_tag(std::string_view sql, const ZephyrResultSet* rs) {
    while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.front()))) {
        sql.remove_prefix(1);
    }
    std::string verb;
    for (char ch : sql) {
        if (std::isspace(static_cast<unsigned char>(ch))) break;
        verb.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (rs->col_count > 0 || verb == "SELECT" || verb == "WITH") {
        return "SELECT " + std::to_string(rs->row_count);
    }
    if (verb == "INSERT") {
        return "INSERT 0 " + std::to_string(rs->rows_affected);
    }
    if (verb == "UPDATE") {
        return "UPDATE " + std::to_string(rs->rows_affected);
    }
    if (verb == "DELETE") {
        return "DELETE " + std::to_string(rs->rows_affected);
    }
    if (!verb.empty()) {
        return verb;
    }
    return "OK";
}

#endif // !__EMSCRIPTEN__

} // namespace

PgWireServer::~PgWireServer() {
    stop();
}

bool PgWireServer::start(ZephyrDB* db, uint16_t port) {
    if (!db) return false;
    return start(
        [db](const char* sql) -> ZephyrResultSet* {
            return zephyr_query(db, sql);
        },
        port,
        [db]() -> const char* {
            return zephyr_last_error(db);
        });
}

bool PgWireServer::start(QueryHandler handler, uint16_t port, ErrorProvider err_provider) {
    stop();

#if defined(__EMSCRIPTEN__)
    (void)handler;
    (void)port;
    (void)err_provider;
    return false;
#else
    if (!handler) return false;
    ensure_wsa();

    SocketHandle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == kInvalidSocket) return false;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(sock, 32) != 0) {
        close_sock(static_cast<uintptr_t>(sock));
        return false;
    }

    sockaddr_in bound{};
#if defined(_WIN32)
    int blen = sizeof(bound);
#else
    socklen_t blen = sizeof(bound);
#endif
    uint16_t bport = port;
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
        bport = ntohs(bound.sin_port);
    }

    query_handler_ = std::move(handler);
    error_provider_ = std::move(err_provider);
    listen_sock_.store(static_cast<uintptr_t>(sock), std::memory_order_release);
    bound_port_.store(bport, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    sched_ = std::make_unique<zephyr::cpp26::execution::thread_pool_scheduler>(4);
    zephyr::cpp26::execution::start_detached(
        zephyr::cpp26::execution::schedule_task(*sched_, [this] { accept_loop(); }));
    return true;
#endif
}

void PgWireServer::stop() {
    bool was = running_.exchange(false, std::memory_order_acq_rel);
    if (!was && !sched_) return;

#if !defined(__EMSCRIPTEN__)
    uintptr_t ls = listen_sock_.exchange(static_cast<uintptr_t>(kInvalidSocket), std::memory_order_acq_rel);
    if (ls != static_cast<uintptr_t>(kInvalidSocket)) {
        close_sock(ls);
    }
    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        for (uintptr_t cs : client_socks_) {
            close_sock(cs);
        }
        client_socks_.clear();
    }
#endif

    if (sched_) {
        sched_->stop();
        sched_.reset();
    }
    bound_port_.store(0, std::memory_order_release);
}

void PgWireServer::accept_loop() {
#if !defined(__EMSCRIPTEN__)
    while (running_.load(std::memory_order_acquire)) {
        auto ls = static_cast<SocketHandle>(listen_sock_.load(std::memory_order_acquire));
        if (ls == kInvalidSocket) break;

        sockaddr_in cli{};
#if defined(_WIN32)
        int clen = sizeof(cli);
#else
        socklen_t clen = sizeof(cli);
#endif
        SocketHandle cs = ::accept(ls, reinterpret_cast<sockaddr*>(&cli), &clen);
        if (cs == kInvalidSocket) {
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        int nodelay = 1;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        uintptr_t raw = static_cast<uintptr_t>(cs);
        {
            std::lock_guard<std::mutex> lk(clients_mu_);
            client_socks_.push_back(raw);
        }

        zephyr::cpp26::execution::start_detached(
            zephyr::cpp26::execution::schedule_task(*sched_, [this, raw] { handle_client(raw); }));
    }
#endif
}

void PgWireServer::handle_client(uintptr_t client_sock) {
#if !defined(__EMSCRIPTEN__)
    auto s = static_cast<SocketHandle>(client_sock);

    // 1. Startup phase (optional SSLRequest followed by StartupMessage)
    while (running_.load(std::memory_order_acquire)) {
        uint8_t hdr[8];
        if (!recv_exact(s, hdr, 8)) {
            close_sock(client_sock);
            return;
        }
        int32_t len = read_be32(hdr);
        int32_t code = read_be32(hdr + 4);
        if (len < 8 || len > 16384) {
            close_sock(client_sock);
            return;
        }
        if (len > 8) {
            std::vector<uint8_t> discard(static_cast<size_t>(len - 8));
            if (!recv_exact(s, discard.data(), discard.size())) {
                close_sock(client_sock);
                return;
            }
        }
        if (code == 80877103) {
            // SSLRequest -> decline with 'N'
            const char n_reply = 'N';
            if (!send_all(s, &n_reply, 1)) {
                close_sock(client_sock);
                return;
            }
            continue;
        }
        // Protocol v3.0 (196608)
        break;
    }

    send_auth_ok(s);
    send_param_status(s, "server_version", "16.0");
    send_param_status(s, "client_encoding", "UTF8");
    send_param_status(s, "standard_conforming_strings", "on");
    send_backend_key(s, next_backend_pid_.fetch_add(1, std::memory_order_relaxed), 4242);
    send_ready_for_query(s, 'I');

    // 2. Message loop
    while (running_.load(std::memory_order_acquire)) {
        uint8_t msg_hdr[5];
        if (!recv_exact(s, msg_hdr, 5)) break;

        char mtype = static_cast<char>(msg_hdr[0]);
        int32_t mlen = read_be32(msg_hdr + 1);
        if (mlen < 4 || mlen > 64 * 1024 * 1024) break;

        std::vector<uint8_t> body(static_cast<size_t>(mlen - 4));
        if (!body.empty() && !recv_exact(s, body.data(), body.size())) break;

        if (mtype == 'X') {
            // Terminate
            break;
        } else if (mtype == 'Q') {
            std::string sql(reinterpret_cast<const char*>(body.data()), body.size());
            while (!sql.empty() && sql.back() == '\0') sql.pop_back();

            bool all_ws = std::all_of(sql.begin(), sql.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            });
            if (all_ws) {
                send_empty_query(s);
                send_ready_for_query(s, 'I');
                continue;
            }

            ZephyrResultSet* rs = query_handler_ ? query_handler_(sql.c_str()) : nullptr;
            if (!rs) {
                const char* err = error_provider_ ? error_provider_() : "Query failed";
                send_error_response(s, err ? err : "Query failed");
                send_ready_for_query(s, 'I');
                continue;
            }

            if (rs->status != ZEPHYR_OK) {
                const char* err = error_provider_ ? error_provider_() : "Query execution error";
                send_error_response(s, (err && err[0] != '\0') ? err : "Query execution error");
                zephyr_result_free(rs);
                send_ready_for_query(s, 'I');
                continue;
            }

            if (rs->col_count > 0 && rs->columns) {
                send_row_description(s, rs);
                if (rs->row_count > 0 && rs->values) {
                    send_data_rows(s, rs);
                }
            }
            send_command_complete(s, build_command_tag(sql, rs));
            zephyr_result_free(rs);
            send_ready_for_query(s, 'I');
        } else {
            send_error_response(s, "Unsupported PGWire message type");
            send_ready_for_query(s, 'I');
        }
    }

    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        client_socks_.erase(
            std::remove(client_socks_.begin(), client_socks_.end(), client_sock),
            client_socks_.end());
    }
    close_sock(client_sock);
#else
    (void)client_sock;
#endif
}

} // namespace zephyr::net
