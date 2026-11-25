#ifndef ZEPHYR_SQL_REFLECTION_HPP
#define ZEPHYR_SQL_REFLECTION_HPP

#include "zephyr/c_abi.h"
#include "zephyr/compat/cpp26.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <string_view>

namespace zephyr::sql::reflection {

template <typename T>
inline std::string struct_to_json(const T& obj) {
    return zephyr::cpp26::reflection::to_json(obj);
}

inline void append_json_escaped(std::ostringstream& os, std::string_view sv) {
    os << '"';
    for (unsigned char c : sv) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\b': os << "\\b"; break;
            case '\f': os << "\\f"; break;
            case '\n': os << "\\n"; break;
            case '\r': os << "\\r"; break;
            case '\t': os << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    os << buf;
                } else {
                    os << static_cast<char>(c);
                }
                break;
        }
    }
    os << '"';
}

inline void append_zephyr_value_json(std::ostringstream& os, const ZephyrValue& val) {
    switch (val.type) {
        case ZEPHYR_TYPE_NULL:
            os << "null";
            break;
        case ZEPHYR_TYPE_I64:
            os << val.as.i64;
            break;
        case ZEPHYR_TYPE_F64:
            if (std::isfinite(val.as.f64)) {
                os << val.as.f64;
            } else {
                os << "null";
            }
            break;
        case ZEPHYR_TYPE_BOOL:
            os << (val.as.boolean ? "true" : "false");
            break;
        case ZEPHYR_TYPE_TEXT:
            if (val.as.slice.ptr && val.as.slice.len > 0) {
                append_json_escaped(
                    os,
                    std::string_view(reinterpret_cast<const char*>(val.as.slice.ptr), val.as.slice.len)
                );
            } else {
                os << "\"\"";
            }
            break;
        case ZEPHYR_TYPE_BYTEA: {
            os << "\"0x";
            static constexpr char kHex[] = "0123456789abcdef";
            for (uint32_t i = 0; i < val.as.slice.len; ++i) {
                uint8_t b = val.as.slice.ptr ? val.as.slice.ptr[i] : 0;
                os << kHex[(b >> 4) & 0xF] << kHex[b & 0xF];
            }
            os << '"';
            break;
        }
        default:
            os << "null";
            break;
    }
}

struct ResultMetaSummary {
    uint64_t row_count{0};
    uint64_t col_count{0};
    uint64_t rows_affected{0};
    uint32_t status{0};
    uint32_t flags{0};
};

ZEPHYR_REFLECT(
    ResultMetaSummary,
    ZEPHYR_FIELD(ResultMetaSummary, row_count),
    ZEPHYR_FIELD(ResultMetaSummary, col_count),
    ZEPHYR_FIELD(ResultMetaSummary, rows_affected),
    ZEPHYR_FIELD(ResultMetaSummary, status),
    ZEPHYR_FIELD(ResultMetaSummary, flags)
)

inline std::string result_set_to_json(const ZephyrResultSet* res) {
    if (!res) {
        return "{\"status\":1,\"rows\":[]}";
    }

    std::ostringstream os;
    ResultMetaSummary meta{
        res->row_count,
        res->col_count,
        res->rows_affected,
        res->status,
        res->flags
    };

    // Use C++26 static reflection for metadata header, then stream rows.
    std::string meta_json = struct_to_json(meta);
    if (!meta_json.empty() && meta_json.back() == '}') {
        meta_json.pop_back();
    }
    os << meta_json << ",\"columns\":[";
    for (uint64_t c = 0; c < res->col_count; ++c) {
        if (c > 0) os << ',';
        std::string_view cname;
        if (res->columns && res->columns[c].name) {
            cname = std::string_view(res->columns[c].name, res->columns[c].name_len);
        }
        os << "{\"name\":";
        append_json_escaped(os, cname);
        os << ",\"type\":" << (res->columns ? res->columns[c].type : 0) << '}';
    }
    os << "],\"rows\":[";
    for (uint64_t r = 0; r < res->row_count; ++r) {
        if (r > 0) os << ',';
        os << '{';
        for (uint64_t c = 0; c < res->col_count; ++c) {
            if (c > 0) os << ',';
            std::string_view cname;
            if (res->columns && res->columns[c].name) {
                cname = std::string_view(res->columns[c].name, res->columns[c].name_len);
            }
            append_json_escaped(os, cname);
            os << ':';
            if (res->values) {
                append_zephyr_value_json(os, res->values[r * res->col_count + c]);
            } else {
                os << "null";
            }
        }
        os << '}';
    }
    os << "]}";
    return os.str();
}

} // namespace zephyr::sql::reflection

#endif // ZEPHYR_SQL_REFLECTION_HPP
