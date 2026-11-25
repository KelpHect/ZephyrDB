#include "zephyr/net/pgwire.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

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

int16_t read_be16(const uint8_t* p) {
    uint16_t u = (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    return static_cast<int16_t>(u);
}

int32_t read_be32(const uint8_t* p) {
    uint32_t u = (static_cast<uint32_t>(p[0]) << 24) |
                 (static_cast<uint32_t>(p[1]) << 16) |
                 (static_cast<uint32_t>(p[2]) << 8) |
                 static_cast<uint32_t>(p[3]);
    return static_cast<int32_t>(u);
}

bool read_cstr_cursor(const std::vector<uint8_t>& buf, size_t& pos, std::string& out) {
    if (pos >= buf.size()) return false;
    const void* nul = std::memchr(buf.data() + pos, 0, buf.size() - pos);
    if (!nul) return false;
    const size_t end = static_cast<const uint8_t*>(nul) - buf.data();
    out.assign(reinterpret_cast<const char*>(buf.data() + pos), end - pos);
    pos = end + 1;
    return true;
}

bool read_be16_cursor(const std::vector<uint8_t>& buf, size_t& pos, int16_t& out) {
    if (pos + 2 > buf.size()) return false;
    out = read_be16(buf.data() + pos);
    pos += 2;
    return true;
}

bool read_be32_cursor(const std::vector<uint8_t>& buf, size_t& pos, int32_t& out) {
    if (pos + 4 > buf.size()) return false;
    out = read_be32(buf.data() + pos);
    pos += 4;
    return true;
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

void send_simple_tag(SocketHandle s, char msg_type) {
    std::vector<uint8_t> out;
    append_msg_header(out, msg_type, 0);
    send_all(s, out.data(), out.size());
}

void send_parameter_description(SocketHandle s, const std::vector<int32_t>& oids) {
    std::vector<uint8_t> body;
    append_be16(body, static_cast<int16_t>(oids.size()));
    for (int32_t oid : oids) {
        append_be32(body, oid);
    }
    std::vector<uint8_t> out;
    append_msg_header(out, 't', body.size());
    out.insert(out.end(), body.begin(), body.end());
    send_all(s, out.data(), out.size());
}

void send_single_text_row(SocketHandle s, std::string_view col_name, std::string_view value) {
    std::vector<uint8_t> tbody;
    append_be16(tbody, 1);
    append_cstr(tbody, col_name);
    append_be32(tbody, 0);
    append_be16(tbody, 0);
    append_be32(tbody, 25); // TEXT
    append_be16(tbody, -1);
    append_be32(tbody, -1);
    append_be16(tbody, 0);
    std::vector<uint8_t> tout;
    append_msg_header(tout, 'T', tbody.size());
    tout.insert(tout.end(), tbody.begin(), tbody.end());
    if (!send_all(s, tout.data(), tout.size())) return;

    std::vector<uint8_t> dbody;
    append_be16(dbody, 1);
    append_be32(dbody, static_cast<int32_t>(value.size()));
    dbody.insert(dbody.end(), value.begin(), value.end());
    std::vector<uint8_t> dout;
    append_msg_header(dout, 'D', dbody.size());
    dout.insert(dout.end(), dbody.begin(), dbody.end());
    if (!send_all(s, dout.data(), dout.size())) return;

    send_command_complete(s, "SELECT 1");
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

void send_data_rows(SocketHandle s, const ZephyrResultSet* rs, int32_t max_rows = 0) {
    const int16_t ncols = static_cast<int16_t>(rs->col_count);
    uint64_t limit = rs->row_count;
    if (max_rows > 0 && static_cast<uint64_t>(max_rows) < limit) {
        limit = static_cast<uint64_t>(max_rows);
    }
    for (uint64_t r = 0; r < limit; ++r) {
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

std::string to_lower_trimmed(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && (std::isspace(static_cast<unsigned char>(sv.back())) || sv.back() == ';')) sv.remove_suffix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    std::string out;
    out.reserve(sv.size());
    bool prev_space = false;
    for (char ch : sv) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch)) {
            if (!prev_space) {
                out.push_back(' ');
                prev_space = true;
            }
        } else {
            out.push_back(static_cast<char>(std::tolower(uch)));
            prev_space = false;
        }
    }
    return out;
}

bool try_handle_benign_query(SocketHandle s, std::string_view sql, int32_t backend_pid) {
    std::string norm = to_lower_trimmed(sql);
    if (norm.empty()) return false;

    if (norm.rfind("set ", 0) == 0) {
        send_command_complete(s, "SET");
        return true;
    }
    if (norm.rfind("reset ", 0) == 0 || norm == "reset all") {
        send_command_complete(s, "RESET");
        return true;
    }
    if (norm.rfind("deallocate ", 0) == 0) {
        send_command_complete(s, "DEALLOCATE");
        return true;
    }
    if (norm.rfind("discard ", 0) == 0) {
        send_command_complete(s, "DISCARD");
        return true;
    }
    if (norm.rfind("show ", 0) == 0) {
        std::string_view var = std::string_view(norm).substr(5);
        if (var == "server_version") {
            send_single_text_row(s, "server_version", "16.0");
        } else if (var == "standard_conforming_strings") {
            send_single_text_row(s, "standard_conforming_strings", "on");
        } else if (var == "client_encoding") {
            send_single_text_row(s, "client_encoding", "UTF8");
        } else if (var == "transaction_isolation") {
            send_single_text_row(s, "transaction_isolation", "read committed");
        } else {
            send_single_text_row(s, var, "");
        }
        return true;
    }
    if (norm == "select version()") {
        send_single_text_row(s, "version", "PostgreSQL 16.0 (ZephyrDB)");
        return true;
    }
    if (norm == "select current_database()") {
        send_single_text_row(s, "current_database", "zephyr");
        return true;
    }
    if (norm == "select current_schema()" || norm == "select current_schema") {
        send_single_text_row(s, "current_schema", "public");
        return true;
    }
    if (norm == "select current_user" || norm == "select user") {
        send_single_text_row(s, "current_user", "zephyr");
        return true;
    }
    if (norm == "select pg_backend_pid()") {
        send_single_text_row(s, "pg_backend_pid", std::to_string(backend_pid));
        return true;
    }
    return false;
}

struct PreparedStmt {
    std::string sql;
    std::vector<int32_t> param_oids;
};

struct BoundPortal {
    std::string sql;
    std::deque<std::string> storage;
    std::vector<ZephyrValue> params;
};

ZephyrValue make_null_val() {
    ZephyrValue v{};
    v.type = ZEPHYR_TYPE_NULL;
    return v;
}

ZephyrValue make_text_val(std::deque<std::string>& storage, std::string_view sv) {
    storage.emplace_back(sv.data(), sv.size());
    const std::string& stored = storage.back();
    ZephyrValue v{};
    v.type = ZEPHYR_TYPE_TEXT;
    v.as.slice.ptr = reinterpret_cast<const uint8_t*>(stored.data());
    v.as.slice.len = static_cast<uint32_t>(stored.size());
    return v;
}

ZephyrValue parse_param_value(
    const uint8_t* data,
    int32_t len,
    int16_t fmt,
    int32_t oid,
    std::deque<std::string>& storage
) {
    if (len < 0) {
        return make_null_val();
    }
    if (fmt == 1) {
        // Binary format
        if (oid == 16 && len == 1) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_BOOL;
            v.as.boolean = data[0] != 0 ? 1 : 0;
            return v;
        }
        if ((oid == 20 && len == 8) || (oid == 23 && len == 4) || (oid == 21 && len == 2)) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_I64;
            if (len == 8) {
                uint64_t u = (static_cast<uint64_t>(data[0]) << 56) |
                             (static_cast<uint64_t>(data[1]) << 48) |
                             (static_cast<uint64_t>(data[2]) << 40) |
                             (static_cast<uint64_t>(data[3]) << 32) |
                             (static_cast<uint64_t>(data[4]) << 24) |
                             (static_cast<uint64_t>(data[5]) << 16) |
                             (static_cast<uint64_t>(data[6]) << 8) |
                             static_cast<uint64_t>(data[7]);
                v.as.i64 = static_cast<int64_t>(u);
            } else if (len == 4) {
                v.as.i64 = static_cast<int64_t>(read_be32(data));
            } else {
                v.as.i64 = static_cast<int64_t>(read_be16(data));
            }
            return v;
        }
        if ((oid == 701 && len == 8) || (oid == 700 && len == 4)) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_F64;
            if (len == 8) {
                uint64_t u = (static_cast<uint64_t>(data[0]) << 56) |
                             (static_cast<uint64_t>(data[1]) << 48) |
                             (static_cast<uint64_t>(data[2]) << 40) |
                             (static_cast<uint64_t>(data[3]) << 32) |
                             (static_cast<uint64_t>(data[4]) << 24) |
                             (static_cast<uint64_t>(data[5]) << 16) |
                             (static_cast<uint64_t>(data[6]) << 8) |
                             static_cast<uint64_t>(data[7]);
                double d = 0.0;
                std::memcpy(&d, &u, sizeof(d));
                v.as.f64 = d;
            } else {
                uint32_t u = static_cast<uint32_t>(read_be32(data));
                float f = 0.0f;
                std::memcpy(&f, &u, sizeof(f));
                v.as.f64 = static_cast<double>(f);
            }
            return v;
        }
        storage.emplace_back(reinterpret_cast<const char*>(data), static_cast<size_t>(len));
        const std::string& stored = storage.back();
        ZephyrValue v{};
        v.type = (oid == 17) ? ZEPHYR_TYPE_BYTEA : ZEPHYR_TYPE_TEXT;
        v.as.slice.ptr = reinterpret_cast<const uint8_t*>(stored.data());
        v.as.slice.len = static_cast<uint32_t>(stored.size());
        return v;
    }

    // Text format (fmt == 0)
    std::string_view sv(reinterpret_cast<const char*>(data), static_cast<size_t>(len));

    if (oid == 16) {
        ZephyrValue v{};
        v.type = ZEPHYR_TYPE_BOOL;
        v.as.boolean = (sv == "t" || sv == "true" || sv == "TRUE" || sv == "1") ? 1 : 0;
        return v;
    }
    if (oid == 20 || oid == 21 || oid == 23) {
        int64_t iv = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), iv);
        if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_I64;
            v.as.i64 = iv;
            return v;
        }
        return make_text_val(storage, sv);
    }
    if (oid == 700 || oid == 701 || oid == 1700) {
        std::string tmp(sv);
        char* endp = nullptr;
        double dv = std::strtod(tmp.c_str(), &endp);
        if (endp == tmp.c_str() + tmp.size() && !tmp.empty()) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_F64;
            v.as.f64 = dv;
            return v;
        }
        return make_text_val(storage, sv);
    }
    if (oid == 25 || oid == 1043 || oid == 1042) {
        return make_text_val(storage, sv);
    }

    // Unspecified OID (oid == 0): infer bool / int64 / double / text cleanly
    if (sv == "true" || sv == "false") {
        ZephyrValue v{};
        v.type = ZEPHYR_TYPE_BOOL;
        v.as.boolean = (sv == "true") ? 1 : 0;
        return v;
    }
    if (!sv.empty()) {
        int64_t iv = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), iv);
        if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
            ZephyrValue v{};
            v.type = ZEPHYR_TYPE_I64;
            v.as.i64 = iv;
            return v;
        }
        if (sv.find('.') != std::string_view::npos ||
            sv.find('e') != std::string_view::npos ||
            sv.find('E') != std::string_view::npos) {
            std::string tmp(sv);
            char* endp = nullptr;
            double dv = std::strtod(tmp.c_str(), &endp);
            if (endp == tmp.c_str() + tmp.size()) {
                ZephyrValue v{};
                v.type = ZEPHYR_TYPE_F64;
                v.as.f64 = dv;
                return v;
            }
        }
    }
    return make_text_val(storage, sv);
}

#endif // !__EMSCRIPTEN__

} // namespace

PgWireServer::~PgWireServer() {
    stop();
}

bool PgWireServer::start(ZephyrDB* db, uint16_t port) {
    if (!db) return false;
    return start_impl(
        db,
        [db](const char* sql) -> ZephyrResultSet* {
            return zephyr_query(db, sql);
        },
        port,
        [db]() -> const char* {
            return zephyr_last_error(db);
        });
}

bool PgWireServer::start(QueryHandler handler, uint16_t port, ErrorProvider err_provider) {
    return start_impl(nullptr, std::move(handler), port, std::move(err_provider));
}

bool PgWireServer::start_impl(ZephyrDB* db, QueryHandler handler, uint16_t port, ErrorProvider err_provider) {
    stop();

#if defined(__EMSCRIPTEN__)
    (void)db;
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

    db_ = db;
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
    db_ = nullptr;
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

    const int32_t backend_pid = next_backend_pid_.fetch_add(1, std::memory_order_relaxed);
    send_auth_ok(s);
    send_param_status(s, "server_version", "16.0");
    send_param_status(s, "client_encoding", "UTF8");
    send_param_status(s, "standard_conforming_strings", "on");
    send_backend_key(s, backend_pid, 4242);
    send_ready_for_query(s, 'I');

    // 2. Message loop (per-connection Extended Query state)
    std::unordered_map<std::string, PreparedStmt> stmts;
    std::unordered_map<std::string, BoundPortal> portals;
    bool in_error = false;

    auto execute_and_send = [&](const std::string& sql, const std::vector<ZephyrValue>& params, int32_t max_rows) -> bool {
        bool all_ws = std::all_of(sql.begin(), sql.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        if (all_ws) {
            send_empty_query(s);
            return true;
        }
        if (try_handle_benign_query(s, sql, backend_pid)) {
            return true;
        }

        ZephyrResultSet* rs = nullptr;
        if (db_) {
            rs = zephyr_query_params(
                db_,
                sql.c_str(),
                params.empty() ? nullptr : params.data(),
                static_cast<uint32_t>(params.size()));
        } else if (query_handler_) {
            rs = query_handler_(sql.c_str());
        }

        if (!rs) {
            const char* err = error_provider_ ? error_provider_() : "Query failed";
            send_error_response(s, err ? err : "Query failed");
            return false;
        }
        if (rs->status != ZEPHYR_OK) {
            const char* err = error_provider_ ? error_provider_() : "Query execution error";
            send_error_response(s, (err && err[0] != '\0') ? err : "Query execution error");
            zephyr_result_free(rs);
            return false;
        }

        if (rs->col_count > 0 && rs->columns) {
            send_row_description(s, rs);
            if (rs->row_count > 0 && rs->values) {
                send_data_rows(s, rs, max_rows);
            }
        }
        send_command_complete(s, build_command_tag(sql, rs));
        zephyr_result_free(rs);
        return true;
    };

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
        } else if (mtype == 'S') {
            // Sync
            in_error = false;
            send_ready_for_query(s, 'I');
        } else if (mtype == 'H') {
            // Flush (no-op; writes are unbuffered)
        } else if (in_error && (mtype == 'P' || mtype == 'B' || mtype == 'D' || mtype == 'E' || mtype == 'C')) {
            // Ignore extended query messages after an error until Sync ('S')
            continue;
        } else if (mtype == 'Q') {
            in_error = false;
            std::string sql(reinterpret_cast<const char*>(body.data()), body.size());
            while (!sql.empty() && sql.back() == '\0') sql.pop_back();
            execute_and_send(sql, {}, 0);
            send_ready_for_query(s, 'I');
        } else if (mtype == 'P') {
            // Parse: stmt_name\0, query_sql\0, int16 param_count, int32 param_oids[]
            size_t pos = 0;
            std::string stmt_name;
            std::string query_sql;
            int16_t param_count = 0;
            if (!read_cstr_cursor(body, pos, stmt_name) ||
                !read_cstr_cursor(body, pos, query_sql) ||
                !read_be16_cursor(body, pos, param_count) ||
                param_count < 0) {
                send_error_response(s, "Malformed Parse message");
                in_error = true;
                continue;
            }
            std::vector<int32_t> oids(static_cast<size_t>(param_count), 0);
            bool ok = true;
            for (int16_t i = 0; i < param_count; ++i) {
                if (!read_be32_cursor(body, pos, oids[static_cast<size_t>(i)])) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                send_error_response(s, "Malformed Parse parameter OIDs");
                in_error = true;
                continue;
            }
            stmts[stmt_name] = PreparedStmt{std::move(query_sql), std::move(oids)};
            send_simple_tag(s, '1'); // ParseComplete
        } else if (mtype == 'B') {
            // Bind: portal_name\0, stmt_name\0, param_fmt_count, param_fmts[], param_count, [len, bytes...], res_fmt_count, res_fmts[]
            size_t pos = 0;
            std::string portal_name;
            std::string stmt_name;
            int16_t n_fmt = 0;
            if (!read_cstr_cursor(body, pos, portal_name) ||
                !read_cstr_cursor(body, pos, stmt_name) ||
                !read_be16_cursor(body, pos, n_fmt) ||
                n_fmt < 0) {
                send_error_response(s, "Malformed Bind message");
                in_error = true;
                continue;
            }
            auto sit = stmts.find(stmt_name);
            if (sit == stmts.end()) {
                send_error_response(s, "Prepared statement does not exist: " + stmt_name);
                in_error = true;
                continue;
            }
            std::vector<int16_t> fmts(static_cast<size_t>(n_fmt), 0);
            bool ok = true;
            for (int16_t i = 0; i < n_fmt; ++i) {
                if (!read_be16_cursor(body, pos, fmts[static_cast<size_t>(i)])) {
                    ok = false;
                    break;
                }
            }
            int16_t n_params = 0;
            if (!ok || !read_be16_cursor(body, pos, n_params) || n_params < 0) {
                send_error_response(s, "Malformed Bind parameters");
                in_error = true;
                continue;
            }

            BoundPortal portal;
            portal.sql = sit->second.sql;
            portal.params.reserve(static_cast<size_t>(n_params));
            for (int16_t i = 0; i < n_params; ++i) {
                int32_t plen = 0;
                if (!read_be32_cursor(body, pos, plen)) {
                    ok = false;
                    break;
                }
                const uint8_t* pdata = nullptr;
                if (plen >= 0) {
                    if (pos + static_cast<size_t>(plen) > body.size()) {
                        ok = false;
                        break;
                    }
                    pdata = body.data() + pos;
                    pos += static_cast<size_t>(plen);
                }
                int16_t fmt = 0;
                if (n_fmt == 1) {
                    fmt = fmts[0];
                } else if (i < n_fmt) {
                    fmt = fmts[static_cast<size_t>(i)];
                }
                int32_t oid = (static_cast<size_t>(i) < sit->second.param_oids.size())
                                  ? sit->second.param_oids[static_cast<size_t>(i)]
                                  : 0;
                portal.params.push_back(parse_param_value(pdata, plen, fmt, oid, portal.storage));
            }
            if (!ok) {
                send_error_response(s, "Truncated Bind parameter value");
                in_error = true;
                continue;
            }
            portals[portal_name] = std::move(portal);
            send_simple_tag(s, '2'); // BindComplete
        } else if (mtype == 'D') {
            // Describe: 'S' or 'P', name\0
            if (body.size() < 2) {
                send_error_response(s, "Malformed Describe message");
                in_error = true;
                continue;
            }
            char kind = static_cast<char>(body[0]);
            size_t pos = 1;
            std::string name;
            if (!read_cstr_cursor(body, pos, name)) {
                send_error_response(s, "Malformed Describe name");
                in_error = true;
                continue;
            }
            if (kind == 'S') {
                auto sit = stmts.find(name);
                if (sit == stmts.end()) {
                    send_error_response(s, "Prepared statement does not exist: " + name);
                    in_error = true;
                    continue;
                }
                send_parameter_description(s, sit->second.param_oids);
                send_simple_tag(s, 'n'); // NoData
            } else if (kind == 'P') {
                auto pit = portals.find(name);
                if (pit == portals.end()) {
                    send_error_response(s, "Portal does not exist: " + name);
                    in_error = true;
                    continue;
                }
                send_simple_tag(s, 'n'); // NoData
            } else {
                send_error_response(s, "Invalid Describe kind");
                in_error = true;
            }
        } else if (mtype == 'E') {
            // Execute: portal_name\0, int32 max_rows
            size_t pos = 0;
            std::string portal_name;
            int32_t max_rows = 0;
            if (!read_cstr_cursor(body, pos, portal_name) ||
                !read_be32_cursor(body, pos, max_rows)) {
                send_error_response(s, "Malformed Execute message");
                in_error = true;
                continue;
            }
            auto pit = portals.find(portal_name);
            if (pit == portals.end()) {
                send_error_response(s, "Portal does not exist: " + portal_name);
                in_error = true;
                continue;
            }
            if (!execute_and_send(pit->second.sql, pit->second.params, max_rows)) {
                in_error = true;
            }
        } else if (mtype == 'C') {
            // Close: 'S' or 'P', name\0
            if (body.size() < 2) {
                send_error_response(s, "Malformed Close message");
                in_error = true;
                continue;
            }
            char kind = static_cast<char>(body[0]);
            size_t pos = 1;
            std::string name;
            if (!read_cstr_cursor(body, pos, name)) {
                send_error_response(s, "Malformed Close name");
                in_error = true;
                continue;
            }
            if (kind == 'S') {
                stmts.erase(name);
            } else if (kind == 'P') {
                portals.erase(name);
            }
            send_simple_tag(s, '3'); // CloseComplete
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
