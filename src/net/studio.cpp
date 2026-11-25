#include "zephyr/net/studio.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(__EMSCRIPTEN__)
  // Emscripten: HTTP server stubbed cleanly.
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

struct StudioWsaGuard {
    StudioWsaGuard() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~StudioWsaGuard() { WSACleanup(); }
};

void ensure_wsa() {
    static StudioWsaGuard guard;
}

void close_sock(uintptr_t raw) {
    auto s = static_cast<SocketHandle>(raw);
    if (s != kInvalidSocket) {
        ::shutdown(s, SD_BOTH);
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

bool send_all(SocketHandle s, const void* src, size_t n) {
    const auto* p = static_cast<const uint8_t*>(src);
    size_t sent = 0;
    while (sent < n) {
        int chunk = static_cast<int>((std::min)(n - sent, static_cast<size_t>(65536)));
        int r = ::send(s, reinterpret_cast<const char*>(p + sent), chunk, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// ============================================================================
// JSON Helpers (zero external dependencies)
// ============================================================================

void append_json_string(std::string& out, std::string_view sv) {
    out.push_back('"');
    for (unsigned char c : sv) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out.append(buf);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

void append_json_value(std::string& out, const ZephyrValue& val) {
    switch (val.type) {
        case ZEPHYR_TYPE_NULL:
            out.append("null");
            break;
        case ZEPHYR_TYPE_I64:
            out.append(std::to_string(val.as.i64));
            break;
        case ZEPHYR_TYPE_F64:
            if (std::isfinite(val.as.f64)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.15g", val.as.f64);
                out.append(buf);
            } else {
                out.append("null");
            }
            break;
        case ZEPHYR_TYPE_BOOL:
            out.append(val.as.boolean ? "true" : "false");
            break;
        case ZEPHYR_TYPE_TEXT: {
            std::string_view sv;
            if (val.as.slice.ptr && val.as.slice.len > 0) {
                sv = std::string_view(reinterpret_cast<const char*>(val.as.slice.ptr), val.as.slice.len);
            }
            append_json_string(out, sv);
            break;
        }
        case ZEPHYR_TYPE_BYTEA: {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string hex = "\\x";
            if (val.as.slice.ptr && val.as.slice.len > 0) {
                hex.reserve(2 + static_cast<size_t>(val.as.slice.len) * 2);
                for (uint32_t i = 0; i < val.as.slice.len; ++i) {
                    uint8_t b = val.as.slice.ptr[i];
                    hex.push_back(kHex[(b >> 4) & 0x0F]);
                    hex.push_back(kHex[b & 0x0F]);
                }
            }
            append_json_string(out, hex);
            break;
        }
        default:
            out.append("null");
            break;
    }
}

std::string value_to_string(const ZephyrValue& val) {
    switch (val.type) {
        case ZEPHYR_TYPE_TEXT:
            if (val.as.slice.ptr && val.as.slice.len > 0) {
                return std::string(reinterpret_cast<const char*>(val.as.slice.ptr), val.as.slice.len);
            }
            return {};
        case ZEPHYR_TYPE_I64:
            return std::to_string(val.as.i64);
        case ZEPHYR_TYPE_BOOL:
            return val.as.boolean ? "true" : "false";
        default:
            return {};
    }
}

int64_t value_to_i64(const ZephyrValue& val, int64_t fallback = 0) {
    if (val.type == ZEPHYR_TYPE_I64) return val.as.i64;
    if (val.type == ZEPHYR_TYPE_F64) return static_cast<int64_t>(val.as.f64);
    if (val.type == ZEPHYR_TYPE_TEXT && val.as.slice.ptr && val.as.slice.len > 0) {
        int64_t parsed = fallback;
        auto* p = reinterpret_cast<const char*>(val.as.slice.ptr);
        auto [end, ec] = std::from_chars(p, p + val.as.slice.len, parsed);
        if (ec == std::errc{} && end == p + val.as.slice.len) return parsed;
    }
    return fallback;
}

bool value_to_bool(const ZephyrValue& val) {
    if (val.type == ZEPHYR_TYPE_BOOL) return val.as.boolean != 0;
    if (val.type == ZEPHYR_TYPE_I64) return val.as.i64 != 0;
    if (val.type == ZEPHYR_TYPE_TEXT && val.as.slice.ptr && val.as.slice.len > 0) {
        std::string_view sv(reinterpret_cast<const char*>(val.as.slice.ptr), val.as.slice.len);
        return sv == "true" || sv == "TRUE" || sv == "t" || sv == "1" || sv == "YES";
    }
    return false;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool parse_json_string_at(std::string_view s, size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= s.size()) return false;
            char esc = s[pos++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > s.size()) return false;
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = s[pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(10 + h - 'a');
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(10 + h - 'A');
                        else return false;
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    out.push_back(esc);
                    break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

std::string trim_ws(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return std::string(sv);
}

// Extracts `"sql"` field from a JSON object body, or returns the raw body if not JSON.
std::string extract_sql_from_body(std::string_view body) {
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.remove_prefix(1);
    while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.remove_suffix(1);
    if (body.empty()) return {};

    if (body.front() != '{') {
        return std::string(body);
    }

    size_t pos = 1;
    auto skip_ws = [&]() {
        while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) ++pos;
    };

    while (pos < body.size()) {
        skip_ws();
        if (pos >= body.size() || body[pos] == '}') break;
        if (body[pos] == ',') {
            ++pos;
            continue;
        }
        std::string key;
        if (!parse_json_string_at(body, pos, key)) break;
        skip_ws();
        if (pos >= body.size() || body[pos] != ':') break;
        ++pos;
        skip_ws();
        if (pos >= body.size()) break;

        if (key == "sql" && body[pos] == '"') {
            std::string sql;
            if (parse_json_string_at(body, pos, sql)) {
                return trim_ws(sql);
            }
            break;
        }

        // Skip other JSON values
        if (body[pos] == '"') {
            std::string dummy;
            if (!parse_json_string_at(body, pos, dummy)) break;
        } else {
            int depth = 0;
            while (pos < body.size()) {
                char c = body[pos];
                if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') {
                    if (depth == 0) break;
                    --depth;
                } else if (c == ',' && depth == 0) {
                    break;
                } else if (c == '"') {
                    std::string dummy;
                    if (!parse_json_string_at(body, pos, dummy)) break;
                    continue;
                }
                ++pos;
            }
        }
    }
    return {};
}

// ============================================================================
// HTTP Request Parsing & Response Serialization
// ============================================================================

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool read_http_request(SocketHandle s, HttpRequest& req) {
    static constexpr size_t kMaxHeaderBytes = 65536;
    static constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;

    std::string buf;
    buf.reserve(4096);
    size_t header_end = std::string::npos;

    char tmp[4096];
    while (header_end == std::string::npos) {
        int r = ::recv(s, tmp, sizeof(tmp), 0);
        if (r <= 0) return false;
        buf.append(tmp, static_cast<size_t>(r));
        header_end = buf.find("\r\n\r\n");
        if (header_end == std::string::npos && buf.size() > kMaxHeaderBytes) {
            return false;
        }
    }

    std::string_view headers_view(buf.data(), header_end);
    size_t line_end = headers_view.find("\r\n");
    std::string_view req_line = (line_end == std::string_view::npos)
        ? headers_view
        : headers_view.substr(0, line_end);

    size_t sp1 = req_line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    size_t sp2 = req_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;

    req.method.assign(req_line.substr(0, sp1));
    std::string_view target = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = target.find('?');
    if (qpos != std::string_view::npos) {
        target = target.substr(0, qpos);
    }
    req.path.assign(target);

    size_t content_length = 0;
    if (line_end != std::string_view::npos) {
        size_t cur = line_end + 2;
        while (cur < headers_view.size()) {
            size_t next = headers_view.find("\r\n", cur);
            std::string_view hline = (next == std::string_view::npos)
                ? headers_view.substr(cur)
                : headers_view.substr(cur, next - cur);
            size_t colon = hline.find(':');
            if (colon != std::string_view::npos) {
                std::string_view hname = hline.substr(0, colon);
                std::string_view hval = hline.substr(colon + 1);
                while (!hval.empty() && std::isspace(static_cast<unsigned char>(hval.front()))) {
                    hval.remove_prefix(1);
                }
                while (!hval.empty() && std::isspace(static_cast<unsigned char>(hval.back()))) {
                    hval.remove_suffix(1);
                }
                if (iequals(hname, "content-length")) {
                    size_t parsed = 0;
                    auto [ptr, ec] = std::from_chars(hval.data(), hval.data() + hval.size(), parsed);
                    if (ec == std::errc{} && ptr == hval.data() + hval.size()) {
                        content_length = parsed;
                    }
                }
            }
            if (next == std::string_view::npos) break;
            cur = next + 2;
        }
    }

    if (content_length > kMaxBodyBytes) {
        return false;
    }

    size_t body_start = header_end + 4;
    req.body.assign(buf.data() + body_start, buf.size() - body_start);
    while (req.body.size() < content_length) {
        size_t need = content_length - req.body.size();
        int to_read = static_cast<int>((std::min)(need, sizeof(tmp)));
        int r = ::recv(s, tmp, to_read, 0);
        if (r <= 0) return false;
        req.body.append(tmp, static_cast<size_t>(r));
    }
    if (req.body.size() > content_length) {
        req.body.resize(content_length);
    }
    return true;
}

void send_http_response(
    SocketHandle s,
    int status_code,
    std::string_view status_text,
    std::string_view content_type,
    std::string_view body
) {
    std::string hdr;
    hdr.reserve(256);
    hdr.append("HTTP/1.1 ");
    hdr.append(std::to_string(status_code));
    hdr.push_back(' ');
    hdr.append(status_text);
    hdr.append("\r\nContent-Type: ");
    hdr.append(content_type);
    hdr.append("\r\nContent-Length: ");
    hdr.append(std::to_string(body.size()));
    hdr.append("\r\nAccess-Control-Allow-Origin: *");
    hdr.append("\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS");
    hdr.append("\r\nAccess-Control-Allow-Headers: Content-Type");
    hdr.append("\r\nCache-Control: no-store");
    hdr.append("\r\nConnection: close\r\n\r\n");

    if (!send_all(s, hdr.data(), hdr.size())) return;
    if (!body.empty()) {
        send_all(s, body.data(), body.size());
    }
}

// ============================================================================
// API Route Handlers
// ============================================================================

std::string handle_api_stats(ZephyrDB* db) {
    ZephyrStats st = zephyr_get_stats(db);
    std::string out;
    out.reserve(256);
    out.append("{\"ok\":true,\"table_count\":");
    out.append(std::to_string(st.table_count));
    out.append(",\"total_rows\":");
    out.append(std::to_string(st.total_rows));
    out.append(",\"wal_lsn\":");
    out.append(std::to_string(st.wal_lsn));
    out.append(",\"wal_flushed_lsn\":");
    out.append(std::to_string(st.wal_flushed_lsn));
    out.append(",\"rcu_reclaimed\":");
    out.append(std::to_string(st.rcu_reclaimed));
    out.append(",\"o1_fastpath_hits\":");
    out.append(std::to_string(st.o1_fastpath_hits));
    out.append(",\"prepared_queries\":");
    out.append(std::to_string(st.prepared_queries));
    out.append(",\"active_txns\":");
    out.append(std::to_string(st.active_txns));
    out.push_back('}');
    return out;
}

std::string handle_api_checkpoint(ZephyrDB* db) {
    int32_t rc = zephyr_checkpoint(db);
    std::string out;
    out.reserve(128);
    if (rc == ZEPHYR_OK) {
        out.append("{\"ok\":true,\"status\":0}");
    } else {
        const char* err = zephyr_last_error(db);
        out.append("{\"ok\":false,\"status\":");
        out.append(std::to_string(rc));
        out.append(",\"error\":");
        append_json_string(out, (err && *err) ? std::string_view(err) : std::string_view("checkpoint failed"));
        out.push_back('}');
    }
    return out;
}

int find_col_idx(const ZephyrResultSet* rs, std::string_view target) {
    if (!rs || !rs->columns) return -1;
    for (uint64_t c = 0; c < rs->col_count; ++c) {
        std::string_view name(rs->columns[c].name ? rs->columns[c].name : "", rs->columns[c].name_len);
        if (iequals(name, target)) return static_cast<int>(c);
    }
    return -1;
}

std::string handle_api_tables(ZephyrDB* db) {
    struct ColInfo {
        std::string name;
        int64_t ordinal_position{0};
        std::string data_type;
        std::string is_nullable;
        bool is_primary_key{false};
    };
    struct TableInfo {
        std::string schema;
        std::string name;
        std::string type;
        int64_t row_count{0};
        std::vector<ColInfo> columns;
    };

    std::vector<TableInfo> tables;
    std::unordered_map<std::string, size_t> table_idx;

    ZephyrResultSet* rs_tables = zephyr_query(
        db,
        "SELECT table_schema, table_name, table_type, row_count FROM information_schema.tables;"
    );
    if (rs_tables) {
        if (rs_tables->status == ZEPHYR_OK) {
            int idx_schema = find_col_idx(rs_tables, "table_schema");
            int idx_name = find_col_idx(rs_tables, "table_name");
            int idx_type = find_col_idx(rs_tables, "table_type");
            int idx_rows = find_col_idx(rs_tables, "row_count");

            for (uint64_t r = 0; r < rs_tables->row_count; ++r) {
                const ZephyrValue* row = rs_tables->values + r * rs_tables->col_count;
                TableInfo ti;
                ti.schema = (idx_schema >= 0) ? value_to_string(row[idx_schema]) : "public";
                ti.name = (idx_name >= 0) ? value_to_string(row[idx_name]) : "";
                ti.type = (idx_type >= 0) ? value_to_string(row[idx_type]) : "BASE TABLE";
                if (idx_rows >= 0) {
                    ti.row_count = value_to_i64(row[idx_rows], 0);
                } else if (!ti.name.empty()) {
                    ZephyrAggregate agg = zephyr_aggregate_count(db, ti.name.c_str());
                    if (agg.status == ZEPHYR_OK) {
                        ti.row_count = agg.count;
                    }
                }
                if (!ti.name.empty()) {
                    table_idx[ti.name] = tables.size();
                    tables.push_back(std::move(ti));
                }
            }
        }
        zephyr_result_free(rs_tables);
    }

    ZephyrResultSet* rs_cols = zephyr_query(
        db,
        "SELECT table_name, column_name, ordinal_position, data_type, is_nullable, is_primary_key "
        "FROM information_schema.columns;"
    );
    if (rs_cols) {
        if (rs_cols->status == ZEPHYR_OK) {
            int idx_tname = find_col_idx(rs_cols, "table_name");
            int idx_cname = find_col_idx(rs_cols, "column_name");
            int idx_ord = find_col_idx(rs_cols, "ordinal_position");
            int idx_dtype = find_col_idx(rs_cols, "data_type");
            int idx_null = find_col_idx(rs_cols, "is_nullable");
            int idx_pk = find_col_idx(rs_cols, "is_primary_key");

            for (uint64_t r = 0; r < rs_cols->row_count; ++r) {
                const ZephyrValue* row = rs_cols->values + r * rs_cols->col_count;
                std::string tname = (idx_tname >= 0) ? value_to_string(row[idx_tname]) : "";
                auto it = table_idx.find(tname);
                if (it == table_idx.end()) continue;

                ColInfo ci;
                ci.name = (idx_cname >= 0) ? value_to_string(row[idx_cname]) : "";
                ci.ordinal_position = (idx_ord >= 0) ? value_to_i64(row[idx_ord], 0) : 0;
                ci.data_type = (idx_dtype >= 0) ? value_to_string(row[idx_dtype]) : "TEXT";
                ci.is_nullable = (idx_null >= 0) ? value_to_string(row[idx_null]) : "YES";
                ci.is_primary_key = (idx_pk >= 0) ? value_to_bool(row[idx_pk]) : false;
                tables[it->second].columns.push_back(std::move(ci));
            }
        }
        zephyr_result_free(rs_cols);
    }

    std::string out;
    out.reserve(1024);
    out.append("{\"ok\":true,\"tables\":[");
    for (size_t i = 0; i < tables.size(); ++i) {
        if (i > 0) out.push_back(',');
        const auto& t = tables[i];
        out.append("{\"schema\":");
        append_json_string(out, t.schema);
        out.append(",\"name\":");
        append_json_string(out, t.name);
        out.append(",\"type\":");
        append_json_string(out, t.type);
        out.append(",\"row_count\":");
        out.append(std::to_string(t.row_count));
        out.append(",\"columns\":[");
        for (size_t c = 0; c < t.columns.size(); ++c) {
            if (c > 0) out.push_back(',');
            const auto& col = t.columns[c];
            out.append("{\"name\":");
            append_json_string(out, col.name);
            out.append(",\"ordinal_position\":");
            out.append(std::to_string(col.ordinal_position));
            out.append(",\"data_type\":");
            append_json_string(out, col.data_type);
            out.append(",\"is_nullable\":");
            append_json_string(out, col.is_nullable);
            out.append(",\"is_primary_key\":");
            out.append(col.is_primary_key ? "true" : "false");
            out.push_back('}');
        }
        out.append("]}");
    }
    out.append("]}");
    return out;
}

std::string handle_api_query(ZephyrDB* db, std::string_view body, int& http_status) {
    std::string sql = extract_sql_from_body(body);
    if (sql.empty()) {
        http_status = 400;
        return "{\"ok\":false,\"error\":\"Empty SQL query\"}";
    }

    auto t0 = std::chrono::steady_clock::now();
    ZephyrResultSet* rs = zephyr_query(db, sql.c_str());
    auto t1 = std::chrono::steady_clock::now();
    int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    if (!rs || rs->status != ZEPHYR_OK) {
        const char* err = zephyr_last_error(db);
        std::string msg = (err && *err) ? std::string(err) : std::string("Query execution failed");
        if (rs) zephyr_result_free(rs);

        http_status = 200;
        std::string out;
        out.reserve(msg.size() + 64);
        out.append("{\"ok\":false,\"elapsed_ns\":");
        out.append(std::to_string(elapsed_ns));
        out.append(",\"error\":");
        append_json_string(out, msg);
        out.push_back('}');
        return out;
    }

    bool o1_fastpath = (rs->flags & ZEPHYR_FLAG_O1_FASTPATH) != 0;

    std::string out;
    out.reserve(512 + static_cast<size_t>(rs->row_count * rs->col_count * 16));
    out.append("{\"ok\":true,\"elapsed_ns\":");
    out.append(std::to_string(elapsed_ns));
    out.append(",\"o1_fastpath\":");
    out.append(o1_fastpath ? "true" : "false");
    out.append(",\"row_count\":");
    out.append(std::to_string(rs->row_count));
    out.append(",\"rows_affected\":");
    out.append(std::to_string(rs->rows_affected));
    out.append(",\"columns\":[");
    for (uint64_t c = 0; c < rs->col_count; ++c) {
        if (c > 0) out.push_back(',');
        const ZephyrColumnDesc& col = rs->columns[c];
        std::string_view cname(col.name ? col.name : "", col.name_len);
        append_json_string(out, cname);
    }
    out.append("],\"rows\":[");
    for (uint64_t r = 0; r < rs->row_count; ++r) {
        if (r > 0) out.push_back(',');
        out.push_back('[');
        for (uint64_t c = 0; c < rs->col_count; ++c) {
            if (c > 0) out.push_back(',');
            append_json_value(out, rs->values[r * rs->col_count + c]);
        }
        out.push_back(']');
    }
    out.append("]}");

    zephyr_result_free(rs);
    http_status = 200;
    return out;
}

// ============================================================================
// Self-Contained Dark-Mode Zephyr Studio Web UI
// Split into chunks well below MSVC's 16,380-byte string literal limit.
// ============================================================================

const std::string& studio_html() {
    static const std::string html = [] {
        static constexpr std::string_view kPart1 = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zephyr Studio — Embedded SQL &amp; Telemetry</title>
<style>
:root{
  --bg:#0B0F19;
  --panel:#0F172A;
  --panel-2:#131C31;
  --border:#1E293B;
  --text:#E2E8F0;
  --muted:#94A3B8;
  --cyan:#22D3EE;
  --indigo:#6366F1;
  --emerald:#10B981;
  --rose:#F43F5E;
  --amber:#F59E0B;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%;background:var(--bg);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif;font-size:13px;overflow:hidden}
.app{display:flex;flex-direction:column;height:100vh}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--border);flex-shrink:0}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:15px;letter-spacing:.2px;white-space:nowrap}
.brand-dot{width:10px;height:10px;border-radius:50%;background:var(--cyan);box-shadow:0 0 10px var(--cyan)}
.brand span{background:linear-gradient(90deg,var(--cyan),var(--indigo));-webkit-background-clip:text;-webkit-text-fill-color:transparent}
.stats{display:flex;align-items:center;gap:8px;flex-wrap:wrap;flex:1;justify-content:center}
.stat-pill{display:flex;align-items:center;gap:6px;background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:5px 10px;font-size:12px}
.stat-label{color:var(--muted);font-size:11px;text-transform:uppercase;letter-spacing:.4px}
.stat-val{color:var(--text);font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-weight:600}
.stat-val.cyan{color:var(--cyan)}
.stat-val.emerald{color:var(--emerald)}
.actions{display:flex;align-items:center;gap:8px}
button{cursor:pointer;border:1px solid var(--border);background:var(--panel-2);color:var(--text);border-radius:8px;padding:6px 12px;font-size:12px;font-weight:600;transition:all .15s ease}
button:hover{border-color:var(--cyan);color:#fff}
button.primary{background:linear-gradient(135deg,var(--indigo),#4F46E5);border-color:var(--indigo);color:#fff}
button.primary:hover{filter:brightness(1.12)}
button.cyan-btn{background:rgba(34,211,238,.12);border-color:rgba(34,211,238,.45);color:var(--cyan)}
button.cyan-btn:hover{background:rgba(34,211,238,.22)}
.workspace{display:flex;flex:1;min-height:0}
.sidebar{width:270px;background:var(--panel);border-right:1px solid var(--border);display:flex;flex-direction:column;flex-shrink:0}
.sidebar-hdr{display:flex;align-items:center;justify-content:space-between;padding:12px 14px;border-bottom:1px solid var(--border);font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.6px;color:var(--muted)}
.tree{flex:1;overflow-y:auto;padding:8px}
.tbl-group{margin-bottom:6px;border:1px solid transparent;border-radius:8px}
.tbl-row{display:flex;align-items:center;justify-content:space-between;padding:7px 9px;border-radius:7px;cursor:pointer;transition:background .12s}
.tbl-row:hover{background:rgba(99,102,241,.14)}
.tbl-name{display:flex;align-items:center;gap:7px;font-weight:600;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tbl-icon{color:var(--cyan);font-size:12px}
.row-badge{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:11px;color:var(--muted);background:var(--bg);border:1px solid var(--border);border-radius:999px;padding:1px 7px}
.col-list{padding:2px 0 6px 22px;display:flex;flex-direction:column;gap:3px}
.col-item{display:flex;align-items:center;justify-content:space-between;gap:6px;padding:3px 8px;border-radius:5px;font-size:12px;color:var(--muted)}
.col-item:hover{background:rgba(255,255,255,.03);color:var(--text)}
.col-left{display:flex;align-items:center;gap:6px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pk-tag{font-size:9px;font-weight:700;color:var(--amber);background:rgba(245,158,11,.14);border:1px solid rgba(245,158,11,.35);border-radius:4px;padding:0 4px}
.col-type{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:10px;color:#64748B}
.empty-tree{padding:16px 10px;color:var(--muted);text-align:center;line-height:1.5}
.main{flex:1;display:flex;flex-direction:column;min-width:0;background:var(--bg)}
.editor-wrap{display:flex;flex-direction:column;border-bottom:1px solid var(--border);background:var(--panel)}
.editor-bar{display:flex;align-items:center;justify-content:space-between;padding:8px 14px;border-bottom:1px solid var(--border);gap:10px}
.editor-title{font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.6px;color:var(--muted)}
.kbd{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:10px;color:var(--muted);background:var(--bg);border:1px solid var(--border);border-radius:4px;padding:1px 5px;margin-left:6px}
textarea#sql{width:100%;height:150px;resize:vertical;min-height:90px;max-height:45vh;background:var(--bg);color:#F8FAFC;border:none;outline:none;padding:12px 16px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:13px;line-height:1.55;tab-size:2}
.result-bar{display:flex;align-items:center;justify-content:space-between;padding:8px 14px;background:var(--panel);border-bottom:1px solid var(--border);gap:10px;flex-wrap:wrap}
.result-meta{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;gap:5px;border-radius:999px;padding:3px 10px;font-size:11px;font-weight:700;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
.badge-latency{background:rgba(34,211,238,.12);border:1px solid rgba(34,211,238,.35);color:var(--cyan)}
.badge-o1{background:rgba(16,185,129,.15);border:1px solid rgba(16,185,129,.5);color:var(--emerald);box-shadow:0 0 12px rgba(16,185,129,.2)}
.badge-rows{background:rgba(99,102,241,.14);border:1px solid rgba(99,102,241,.35);color:#A5B4FC}
.view-toggle{display:flex;background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:2px}
.view-toggle button{border:none;background:transparent;padding:4px 10px;border-radius:6px;color:var(--muted);font-size:11px}
.view-toggle button.active{background:var(--indigo);color:#fff}
.results{flex:1;overflow:auto;padding:0;position:relative}
table.grid{width:100%;border-collapse:collapse;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}
table.grid thead th{position:sticky;top:0;background:var(--panel-2);color:var(--cyan);text-align:left;padding:9px 14px;border-bottom:1px solid var(--border);font-weight:600;white-space:nowrap;z-index:1}
table.grid tbody td{padding:8px 14px;border-bottom:1px solid rgba(30,41,59,.65);color:var(--text);white-space:nowrap;max-width:420px;overflow:hidden;text-overflow:ellipsis}
table.grid tbody tr:hover td{background:rgba(99,102,241,.08)}
.null-val{color:#64748B;font-style:italic}
.num-val{color:#93C5FD}
.bool-val{color:#34D399}
pre.json-view{padding:16px;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px;line-height:1.55;color:#CBD5E1;white-space:pre-wrap;word-break:break-word}
.error-box{margin:16px;padding:12px 16px;border-radius:8px;background:rgba(244,63,94,.12);border:1px solid rgba(244,63,94,.45);color:#FDA4AF;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:12px}
.placeholder{padding:36px 20px;text-align:center;color:var(--muted)}
.toast{position:fixed;bottom:16px;right:16px;background:var(--panel-2);border:1px solid var(--cyan);color:var(--text);padding:8px 14px;border-radius:8px;font-size:12px;box-shadow:0 8px 24px rgba(0,0,0,.45);opacity:0;pointer-events:none;transition:opacity .2s}
.toast.show{opacity:1}
</style>
</head>
<body>
<div class="app">
  <header class="topbar">
    <div class="brand"><div class="brand-dot"></div><span>Zephyr Studio</span></div>
    <div class="stats">
      <div class="stat-pill"><span class="stat-label">Tables</span><span class="stat-val cyan" id="st-tables">-</span></div>
      <div class="stat-pill"><span class="stat-label">Rows</span><span class="stat-val" id="st-rows">-</span></div>
      <div class="stat-pill"><span class="stat-label">WAL LSN</span><span class="stat-val" id="st-wal">-</span></div>
      <div class="stat-pill"><span class="stat-label">RCU Reclaimed</span><span class="stat-val" id="st-rcu">-</span></div>
      <div class="stat-pill"><span class="stat-label">O(1) Hits</span><span class="stat-val emerald" id="st-o1">-</span></div>
      <div class="stat-pill"><span class="stat-label">Prepared</span><span class="stat-val" id="st-prep">-</span></div>
    </div>
    <div class="actions">
      <button id="btn-refresh" title="Refresh schema &amp; telemetry">Refresh</button>
      <button class="cyan-btn" id="btn-checkpoint" title="Flush WAL and compact snapshot">Checkpoint</button>
    </div>
  </header>
  <div class="workspace">
    <aside class="sidebar">
      <div class="sidebar-hdr"><span>Schema Explorer</span><span id="tbl-count-lbl">0 tables</span></div>
      <div class="tree" id="schema-tree"><div class="empty-tree">Loading schema...</div></div>
    </aside>
    <main class="main">
      <section class="editor-wrap">
        <div class="editor-bar">
          <div><span class="editor-title">SQL Editor</span><span class="kbd">Ctrl+Enter</span></div>
          <div class="actions">
            <button id="btn-clear">Clear</button>
            <button class="primary" id="btn-run">Run Query</button>
          </div>
        </div>
        <textarea id="sql" spellcheck="false" placeholder="SELECT * FROM information_schema.tables;">SELECT * FROM information_schema.tables;</textarea>
      </section>
      <div class="result-bar">
        <div class="result-meta" id="result-meta"><span class="stat-label">Ready</span></div>
        <div class="view-toggle">
          <button id="tab-table" class="active">Table</button>
          <button id="tab-json">JSON</button>
        </div>
      </div>
      <section class="results" id="results"><div class="placeholder">Run a SQL query or click any table in the Schema Explorer to inspect rows.</div></section>
    </main>
  </div>
</div>
<div class="toast" id="toast"></div>
)html";

        static constexpr std::string_view kPart2 = R"html(<script>
(function(){
  const $ = id => document.getElementById(id);
  let lastResp = null;
  let viewMode = 'table';

  function toast(msg){
    const t = $('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(t._tid);
    t._tid = setTimeout(() => t.classList.remove('show'), 2200);
  }

  function fmtLatency(ns){
    if(ns == null) return '-';
    if(ns < 1000) return ns + ' ns';
    if(ns < 1000000) return (ns / 1000).toFixed(2) + ' \u00b5s';
    return (ns / 1000000).toFixed(2) + ' ms';
  }

  function escHtml(s){
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
  }

  async function loadStats(){
    try{
      const r = await fetch('/api/stats');
      const d = await r.json();
      if(!d.ok) return;
      $('st-tables').textContent = d.table_count;
      $('st-rows').textContent = d.total_rows;
      $('st-wal').textContent = d.wal_lsn + ' / ' + d.wal_flushed_lsn;
      $('st-rcu').textContent = d.rcu_reclaimed;
      $('st-o1').textContent = d.o1_fastpath_hits;
      $('st-prep').textContent = d.prepared_queries;
    }catch(_){}
  }

  async function loadTables(){
    try{
      const r = await fetch('/api/tables');
      const d = await r.json();
      const tree = $('schema-tree');
      const tables = (d && d.tables) ? d.tables : [];
      $('tbl-count-lbl').textContent = tables.length + (tables.length === 1 ? ' table' : ' tables');
      if(!tables.length){
        tree.innerHTML = '<div class="empty-tree">No tables yet.<br>Run a <code>CREATE TABLE</code> statement to get started.</div>';
        return;
      }
      tree.innerHTML = '';
      for(const t of tables){
        const grp = document.createElement('div');
        grp.className = 'tbl-group';
        const row = document.createElement('div');
        row.className = 'tbl-row';
        row.innerHTML = '<div class="tbl-name"><span class="tbl-icon">\u25a6</span><span>' + escHtml(t.name) + '</span></div>' +
                        '<span class="row-badge">' + escHtml(t.row_count) + '</span>';
        row.onclick = () => {
          $('sql').value = 'SELECT * FROM ' + t.name + ' LIMIT 100;';
          runQuery();
        };
        grp.appendChild(row);
        if(t.columns && t.columns.length){
          const cl = document.createElement('div');
          cl.className = 'col-list';
          for(const c of t.columns){
            const ci = document.createElement('div');
            ci.className = 'col-item';
            ci.innerHTML = '<div class="col-left"><span>' + escHtml(c.name) + '</span>' +
                           (c.is_primary_key ? '<span class="pk-tag">PK</span>' : '') +
                           '</div><span class="col-type">' + escHtml(c.data_type) + '</span>';
            cl.appendChild(ci);
          }
          grp.appendChild(cl);
        }
        tree.appendChild(grp);
      }
    }catch(_){
      $('schema-tree').innerHTML = '<div class="empty-tree">Failed to load schema.</div>';
    }
  }

  function renderResult(){
    const meta = $('result-meta');
    const container = $('results');
    if(!lastResp){
      meta.innerHTML = '<span class="stat-label">Ready</span>';
      container.innerHTML = '<div class="placeholder">Run a SQL query or click any table in the Schema Explorer to inspect rows.</div>';
      return;
    }
    const badges = [];
    if(lastResp.elapsed_ns != null){
      badges.push('<span class="badge badge-latency">\u23f1 ' + fmtLatency(lastResp.elapsed_ns) + '</span>');
    }
    if(lastResp.o1_fastpath){
      badges.push('<span class="badge badge-o1">\u26a1 O(1) Fastpath</span>');
    }
    if(lastResp.ok){
      const colCnt = (lastResp.columns || []).length;
      if(colCnt > 0){
        badges.push('<span class="badge badge-rows">' + lastResp.row_count + ' rows</span>');
      } else {
        badges.push('<span class="badge badge-rows">' + lastResp.rows_affected + ' affected</span>');
      }
    }
    meta.innerHTML = badges.join('');

    if(viewMode === 'json'){
      container.innerHTML = '<pre class="json-view">' + escHtml(JSON.stringify(lastResp, null, 2)) + '</pre>';
      return;
    }
    if(!lastResp.ok){
      container.innerHTML = '<div class="error-box">ERROR: ' + escHtml(lastResp.error || 'Unknown error') + '</div>';
      return;
    }
    const cols = lastResp.columns || [];
    const rows = lastResp.rows || [];
    if(cols.length === 0){
      container.innerHTML = '<div class="placeholder">Statement succeeded. Rows affected: <strong>' + escHtml(lastResp.rows_affected) + '</strong></div>';
      return;
    }
    let html = '<table class="grid"><thead><tr>';
    for(const c of cols) html += '<th>' + escHtml(c) + '</th>';
    html += '</tr></thead><tbody>';
    if(rows.length === 0){
      html += '<tr><td colspan="' + cols.length + '" class="null-val">0 rows returned</td></tr>';
    } else {
      for(const r of rows){
        html += '<tr>';
        for(const cell of r){
          if(cell === null || cell === undefined){
            html += '<td class="null-val">NULL</td>';
          } else if(typeof cell === 'number'){
            html += '<td class="num-val">' + escHtml(cell) + '</td>';
          } else if(typeof cell === 'boolean'){
            html += '<td class="bool-val">' + (cell ? 'true' : 'false') + '</td>';
          } else {
            html += '<td>' + escHtml(cell) + '</td>';
          }
        }
        html += '</tr>';
      }
    }
    html += '</tbody></table>';
    container.innerHTML = html;
  }

  async function runQuery(){
    const sql = $('sql').value.trim();
    if(!sql) return;
    try{
      const r = await fetch('/api/query', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({sql})
      });
      lastResp = await r.json();
      renderResult();
      loadStats();
      loadTables();
    }catch(err){
      lastResp = {ok:false, error: String(err)};
      renderResult();
    }
  }

  $('btn-run').onclick = runQuery;
  $('btn-clear').onclick = () => { $('sql').value = ''; $('sql').focus(); };
  $('btn-refresh').onclick = () => { loadStats(); loadTables(); toast('Refreshed telemetry & schema'); };
  $('btn-checkpoint').onclick = async () => {
    try{
      const r = await fetch('/api/checkpoint', {method:'POST'});
      const d = await r.json();
      toast(d.ok ? 'Checkpoint complete' : ('Checkpoint failed: ' + (d.error || d.status)));
      loadStats();
    }catch(err){
      toast('Checkpoint error: ' + err);
    }
  };
  $('tab-table').onclick = () => {
    viewMode = 'table';
    $('tab-table').classList.add('active');
    $('tab-json').classList.remove('active');
    renderResult();
  };
  $('tab-json').onclick = () => {
    viewMode = 'json';
    $('tab-json').classList.add('active');
    $('tab-table').classList.remove('active');
    renderResult();
  };
  $('sql').addEventListener('keydown', e => {
    if((e.ctrlKey || e.metaKey) && e.key === 'Enter'){
      e.preventDefault();
      runQuery();
    }
  });

  loadStats();
  loadTables();
  setInterval(loadStats, 2500);
})();
</script>
</body>
</html>
)html";

        std::string combined;
        combined.reserve(kPart1.size() + kPart2.size());
        combined.append(kPart1);
        combined.append(kPart2);
        return combined;
    }();
    return html;
}

#endif // !__EMSCRIPTEN__

} // namespace

StudioServer::~StudioServer() {
    stop();
}

bool StudioServer::start(ZephyrDB* db, uint16_t port) {
    stop();

#if defined(__EMSCRIPTEN__)
    (void)db;
    (void)port;
    return false;
#else
    if (!db) return false;
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
    listen_sock_.store(static_cast<uintptr_t>(sock), std::memory_order_release);
    bound_port_.store(bport, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    sched_ = std::make_unique<zephyr::cpp26::execution::thread_pool_scheduler>(4);
    zephyr::cpp26::execution::start_detached(
        zephyr::cpp26::execution::schedule_task(*sched_, [this] { accept_loop(); }));
    return true;
#endif
}

void StudioServer::stop() {
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

void StudioServer::remove_client(uintptr_t client_sock) {
    std::lock_guard<std::mutex> lk(clients_mu_);
    auto it = std::find(client_socks_.begin(), client_socks_.end(), client_sock);
    if (it != client_socks_.end()) {
        client_socks_.erase(it);
    }
}

void StudioServer::accept_loop() {
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

void StudioServer::handle_client(uintptr_t client_sock) {
#if !defined(__EMSCRIPTEN__)
    auto s = static_cast<SocketHandle>(client_sock);
    HttpRequest req;

    if (read_http_request(s, req) && running_.load(std::memory_order_acquire) && db_) {
        if (req.method == "OPTIONS") {
            send_http_response(s, 204, "No Content", "text/plain", "");
        } else if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
            send_http_response(s, 200, "OK", "text/html; charset=utf-8", studio_html());
        } else if (req.method == "GET" && req.path == "/api/tables") {
            std::string json = handle_api_tables(db_);
            send_http_response(s, 200, "OK", "application/json; charset=utf-8", json);
        } else if (req.method == "POST" && req.path == "/api/query") {
            int status = 200;
            std::string json = handle_api_query(db_, req.body, status);
            send_http_response(
                s,
                status,
                status == 200 ? "OK" : "Bad Request",
                "application/json; charset=utf-8",
                json
            );
        } else if (req.method == "GET" && req.path == "/api/stats") {
            std::string json = handle_api_stats(db_);
            send_http_response(s, 200, "OK", "application/json; charset=utf-8", json);
        } else if (req.method == "POST" && req.path == "/api/checkpoint") {
            std::string json = handle_api_checkpoint(db_);
            send_http_response(s, 200, "OK", "application/json; charset=utf-8", json);
        } else {
            send_http_response(
                s,
                404,
                "Not Found",
                "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"Not Found\"}"
            );
        }
    }

    remove_client(client_sock);
    close_sock(client_sock);
#else
    (void)client_sock;
#endif
}

} // namespace zephyr::net
