#include <node_api.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "zephyr/c_abi.h"

namespace {

struct DbHandle {
    ZephyrDB* db = nullptr;
};

struct StmtHandle {
    ZephyrStmt* stmt = nullptr;
};

void db_finalizer(napi_env, void* data, void*) {
    auto* h = static_cast<DbHandle*>(data);
    if (h) {
        if (h->db) {
            zephyr_db_close(h->db);
            h->db = nullptr;
        }
        delete h;
    }
}

void stmt_finalizer(napi_env, void* data, void*) {
    auto* h = static_cast<StmtHandle*>(data);
    if (h) {
        if (h->stmt) {
            zephyr_stmt_free(h->stmt);
            h->stmt = nullptr;
        }
        delete h;
    }
}

napi_value throw_err(napi_env env, const char* msg) {
    napi_throw_error(env, nullptr, msg ? msg : "ZephyrDB error");
    return nullptr;
}

std::string get_utf8(napi_env env, napi_value val) {
    size_t len = 0;
    napi_get_value_string_utf8(env, val, nullptr, 0, &len);
    std::string out(len, '\0');
    if (len > 0) {
        napi_get_value_string_utf8(env, val, out.data(), len + 1, &len);
    }
    return out;
}

bool js_to_zephyr_params(
    napi_env env,
    napi_value arr,
    std::deque<std::string>& storage,
    std::vector<ZephyrValue>& out
) {
    bool is_arr = false;
    if (napi_is_array(env, arr, &is_arr) != napi_ok || !is_arr) {
        return false;
    }
    uint32_t len = 0;
    napi_get_array_length(env, arr, &len);
    out.reserve(len);

    for (uint32_t i = 0; i < len; ++i) {
        napi_value elem{};
        napi_get_element(env, arr, i, &elem);
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, elem, &vt);

        ZephyrValue zv{};
        if (vt == napi_null || vt == napi_undefined) {
            zv.type = ZEPHYR_TYPE_NULL;
        } else if (vt == napi_boolean) {
            bool b = false;
            napi_get_value_bool(env, elem, &b);
            zv.type = ZEPHYR_TYPE_BOOL;
            zv.as.boolean = b ? 1 : 0;
        } else if (vt == napi_number) {
            double d = 0.0;
            napi_get_value_double(env, elem, &d);
            double intpart = 0.0;
            if (std::isfinite(d) && std::modf(d, &intpart) == 0.0 &&
                d >= -9007199254740991.0 && d <= 9007199254740991.0) {
                zv.type = ZEPHYR_TYPE_I64;
                zv.as.i64 = static_cast<int64_t>(d);
            } else {
                zv.type = ZEPHYR_TYPE_F64;
                zv.as.f64 = d;
            }
        } else if (vt == napi_bigint) {
            int64_t iv = 0;
            bool lossless = false;
            napi_get_value_bigint_int64(env, elem, &iv, &lossless);
            zv.type = ZEPHYR_TYPE_I64;
            zv.as.i64 = iv;
        } else if (vt == napi_string) {
            storage.push_back(get_utf8(env, elem));
            const std::string& s = storage.back();
            zv.type = ZEPHYR_TYPE_TEXT;
            zv.as.slice.ptr = reinterpret_cast<const uint8_t*>(s.data());
            zv.as.slice.len = static_cast<uint32_t>(s.size());
        } else {
            bool is_buf = false;
            napi_is_buffer(env, elem, &is_buf);
            if (is_buf) {
                void* data = nullptr;
                size_t blen = 0;
                napi_get_buffer_info(env, elem, &data, &blen);
                storage.emplace_back(static_cast<const char*>(data), blen);
                const std::string& s = storage.back();
                zv.type = ZEPHYR_TYPE_BYTEA;
                zv.as.slice.ptr = reinterpret_cast<const uint8_t*>(s.data());
                zv.as.slice.len = static_cast<uint32_t>(s.size());
            } else {
                napi_value str_val{};
                napi_coerce_to_string(env, elem, &str_val);
                storage.push_back(get_utf8(env, str_val));
                const std::string& s = storage.back();
                zv.type = ZEPHYR_TYPE_TEXT;
                zv.as.slice.ptr = reinterpret_cast<const uint8_t*>(s.data());
                zv.as.slice.len = static_cast<uint32_t>(s.size());
            }
        }
        out.push_back(zv);
    }
    return true;
}

napi_value zephyr_val_to_js(napi_env env, const ZephyrValue& v) {
    napi_value out{};
    switch (v.type) {
        case ZEPHYR_TYPE_NULL:
            napi_get_null(env, &out);
            return out;
        case ZEPHYR_TYPE_I64:
            napi_create_int64(env, v.as.i64, &out);
            return out;
        case ZEPHYR_TYPE_F64:
            napi_create_double(env, v.as.f64, &out);
            return out;
        case ZEPHYR_TYPE_BOOL:
            napi_get_boolean(env, v.as.boolean != 0, &out);
            return out;
        case ZEPHYR_TYPE_TEXT:
            napi_create_string_utf8(
                env,
                v.as.slice.ptr ? reinterpret_cast<const char*>(v.as.slice.ptr) : "",
                v.as.slice.len,
                &out);
            return out;
        case ZEPHYR_TYPE_BYTEA: {
            void* dst = nullptr;
            napi_create_buffer_copy(env, v.as.slice.len, v.as.slice.ptr, &dst, &out);
            return out;
        }
        default:
            napi_get_null(env, &out);
            return out;
    }
}

napi_value result_set_to_js(napi_env env, ZephyrDB* db, ZephyrResultSet* rs) {
    if (!rs) {
        return throw_err(env, db ? zephyr_last_error(db) : "Query failed");
    }
    if (rs->status != ZEPHYR_OK) {
        std::string msg = (db && zephyr_last_error(db)) ? zephyr_last_error(db) : "Query execution error";
        zephyr_result_free(rs);
        return throw_err(env, msg.c_str());
    }

    napi_value res{};
    napi_create_object(env, &res);

    napi_value rc{};
    napi_create_int64(env, static_cast<int64_t>(rs->row_count), &rc);
    napi_set_named_property(env, res, "rowCount", rc);

    napi_value cc{};
    napi_create_int64(env, static_cast<int64_t>(rs->col_count), &cc);
    napi_set_named_property(env, res, "colCount", cc);

    napi_value ra{};
    napi_create_int64(env, static_cast<int64_t>(rs->rows_affected), &ra);
    napi_set_named_property(env, res, "rowsAffected", ra);

    napi_value cols{};
    napi_create_array_with_length(env, static_cast<size_t>(rs->col_count), &cols);
    std::vector<std::string> col_names(static_cast<size_t>(rs->col_count));
    for (uint64_t c = 0; c < rs->col_count; ++c) {
        const ZephyrColumnDesc& cd = rs->columns[c];
        col_names[c] = std::string(cd.name ? cd.name : "", cd.name_len);
        napi_value cobj{};
        napi_create_object(env, &cobj);
        napi_value cname{};
        napi_create_string_utf8(env, col_names[c].data(), col_names[c].size(), &cname);
        napi_value ctype{};
        napi_create_int32(env, cd.type, &ctype);
        napi_set_named_property(env, cobj, "name", cname);
        napi_set_named_property(env, cobj, "type", ctype);
        napi_set_element(env, cols, static_cast<uint32_t>(c), cobj);
    }
    napi_set_named_property(env, res, "columns", cols);

    napi_value rows{};
    napi_create_array_with_length(env, static_cast<size_t>(rs->row_count), &rows);
    for (uint64_t r = 0; r < rs->row_count; ++r) {
        napi_value robj{};
        napi_create_object(env, &robj);
        for (uint64_t c = 0; c < rs->col_count; ++c) {
            napi_value val = zephyr_val_to_js(env, rs->values[r * rs->col_count + c]);
            napi_set_named_property(env, robj, col_names[c].c_str(), val);
        }
        napi_set_element(env, rows, static_cast<uint32_t>(r), robj);
    }
    napi_set_named_property(env, res, "rows", rows);

    zephyr_result_free(rs);
    return res;
}

DbHandle* unwrap_db(napi_env env, napi_value val) {
    void* ptr = nullptr;
    if (napi_get_value_external(env, val, &ptr) != napi_ok || !ptr) {
        throw_err(env, "Invalid ZephyrDB handle");
        return nullptr;
    }
    auto* h = static_cast<DbHandle*>(ptr);
    if (!h->db) {
        throw_err(env, "ZephyrDB handle is closed");
        return nullptr;
    }
    return h;
}

StmtHandle* unwrap_stmt(napi_env env, napi_value val) {
    void* ptr = nullptr;
    if (napi_get_value_external(env, val, &ptr) != napi_ok || !ptr) {
        throw_err(env, "Invalid ZephyrStmt handle");
        return nullptr;
    }
    auto* h = static_cast<StmtHandle*>(ptr);
    if (!h->stmt) {
        throw_err(env, "ZephyrStmt handle is freed");
        return nullptr;
    }
    return h;
}

napi_value napi_open(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    ZephyrConfig cfg = zephyr_default_config();
    std::string data_dir;
    if (argc >= 1) {
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, args[0], &vt);
        if (vt == napi_string) {
            data_dir = get_utf8(env, args[0]);
            cfg.data_dir = data_dir.c_str();
        } else if (vt == napi_object) {
            bool has = false;
            napi_has_named_property(env, args[0], "dataDir", &has);
            if (has) {
                napi_value v{};
                napi_get_named_property(env, args[0], "dataDir", &v);
                data_dir = get_utf8(env, v);
                cfg.data_dir = data_dir.c_str();
            }
            napi_has_named_property(env, args[0], "enableWal", &has);
            if (has) {
                napi_value v{};
                napi_get_named_property(env, args[0], "enableWal", &v);
                bool b = true;
                napi_get_value_bool(env, v, &b);
                cfg.enable_wal = b ? 1 : 0;
            }
            napi_has_named_property(env, args[0], "enableCompactor", &has);
            if (has) {
                napi_value v{};
                napi_get_named_property(env, args[0], "enableCompactor", &v);
                bool b = true;
                napi_get_value_bool(env, v, &b);
                cfg.enable_compactor = b ? 1 : 0;
            }
        }
    }

    ZephyrDB* db = zephyr_db_open(&cfg);
    if (!db) {
        return throw_err(env, "Failed to open ZephyrDB");
    }
    auto* h = new DbHandle{db};
    napi_value ext{};
    napi_create_external(env, h, db_finalizer, nullptr, &ext);
    return ext;
}

napi_value napi_close(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return throw_err(env, "Expected db handle");

    void* ptr = nullptr;
    if (napi_get_value_external(env, args[0], &ptr) == napi_ok && ptr) {
        auto* h = static_cast<DbHandle*>(ptr);
        if (h->db) {
            zephyr_db_close(h->db);
            h->db = nullptr;
        }
    }
    napi_value undef{};
    napi_get_undefined(env, &undef);
    return undef;
}

napi_value napi_exec_fn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return throw_err(env, "Expected (db, sql)");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    std::string sql = get_utf8(env, args[1]);
    uint64_t rows_affected = 0;
    int32_t st = zephyr_exec(h->db, sql.c_str(), &rows_affected);
    if (st != ZEPHYR_OK) {
        return throw_err(env, zephyr_last_error(h->db));
    }
    napi_value out{};
    napi_create_int64(env, static_cast<int64_t>(rows_affected), &out);
    return out;
}

napi_value napi_query_fn(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return throw_err(env, "Expected (db, sql, [params])");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    std::string sql = get_utf8(env, args[1]);

    ZephyrResultSet* rs = nullptr;
    if (argc >= 3) {
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, args[2], &vt);
        if (vt != napi_undefined && vt != napi_null) {
            std::deque<std::string> storage;
            std::vector<ZephyrValue> params;
            if (!js_to_zephyr_params(env, args[2], storage, params)) {
                return throw_err(env, "params must be an Array");
            }
            rs = zephyr_query_params(
                h->db,
                sql.c_str(),
                params.empty() ? nullptr : params.data(),
                static_cast<uint32_t>(params.size()));
        } else {
            rs = zephyr_query(h->db, sql.c_str());
        }
    } else {
        rs = zephyr_query(h->db, sql.c_str());
    }
    return result_set_to_js(env, h->db, rs);
}

napi_value napi_prepare_fn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return throw_err(env, "Expected (db, sql)");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    std::string sql = get_utf8(env, args[1]);

    ZephyrStmt* stmt = zephyr_prepare(h->db, sql.c_str());
    if (!stmt) {
        return throw_err(env, zephyr_last_error(h->db));
    }
    auto* sh = new StmtHandle{stmt};
    napi_value ext{};
    napi_create_external(env, sh, stmt_finalizer, nullptr, &ext);
    return ext;
}

napi_value napi_stmt_query_fn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return throw_err(env, "Expected (stmt, [params])");

    StmtHandle* sh = unwrap_stmt(env, args[0]);
    if (!sh) return nullptr;

    std::deque<std::string> storage;
    std::vector<ZephyrValue> params;
    if (argc >= 2) {
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, args[1], &vt);
        if (vt != napi_undefined && vt != napi_null) {
            if (!js_to_zephyr_params(env, args[1], storage, params)) {
                return throw_err(env, "params must be an Array");
            }
        }
    }
    ZephyrResultSet* rs = zephyr_stmt_query(
        sh->stmt,
        params.empty() ? nullptr : params.data(),
        static_cast<uint32_t>(params.size()));
    return result_set_to_js(env, nullptr, rs);
}

napi_value napi_stmt_exec_fn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return throw_err(env, "Expected (stmt, [params])");

    StmtHandle* sh = unwrap_stmt(env, args[0]);
    if (!sh) return nullptr;

    std::deque<std::string> storage;
    std::vector<ZephyrValue> params;
    if (argc >= 2) {
        napi_valuetype vt = napi_undefined;
        napi_typeof(env, args[1], &vt);
        if (vt != napi_undefined && vt != napi_null) {
            if (!js_to_zephyr_params(env, args[1], storage, params)) {
                return throw_err(env, "params must be an Array");
            }
        }
    }
    uint64_t rows_affected = 0;
    int32_t st = zephyr_stmt_exec(
        sh->stmt,
        params.empty() ? nullptr : params.data(),
        static_cast<uint32_t>(params.size()),
        &rows_affected);
    if (st != ZEPHYR_OK) {
        return throw_err(env, "Statement execution failed");
    }
    napi_value out{};
    napi_create_int64(env, static_cast<int64_t>(rows_affected), &out);
    return out;
}

napi_value napi_aggregate_count_fn(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return throw_err(env, "Expected (db, tableName)");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    std::string table = get_utf8(env, args[1]);
    ZephyrAggregate agg = zephyr_aggregate_count(h->db, table.c_str());
    if (agg.status != ZEPHYR_OK) {
        return throw_err(env, zephyr_last_error(h->db));
    }
    napi_value out{};
    napi_create_int64(env, agg.count, &out);
    return out;
}

napi_value napi_aggregate_sum_fn(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 3) return throw_err(env, "Expected (db, tableName, colName)");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    std::string table = get_utf8(env, args[1]);
    std::string col = get_utf8(env, args[2]);
    ZephyrAggregate agg = zephyr_aggregate_sum(h->db, table.c_str(), col.c_str());
    if (agg.status != ZEPHYR_OK) {
        return throw_err(env, zephyr_last_error(h->db));
    }
    napi_value out{};
    napi_create_object(env, &out);
    napi_value cnt{}, si{}, sf{};
    napi_create_int64(env, agg.count, &cnt);
    napi_create_int64(env, agg.sum_i64, &si);
    napi_create_double(env, agg.sum_f64, &sf);
    napi_set_named_property(env, out, "count", cnt);
    napi_set_named_property(env, out, "sumI64", si);
    napi_set_named_property(env, out, "sumF64", sf);
    return out;
}

napi_value napi_checkpoint_fn(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1]{};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) return throw_err(env, "Expected db handle");

    DbHandle* h = unwrap_db(env, args[0]);
    if (!h) return nullptr;
    int32_t st = zephyr_checkpoint(h->db);
    if (st != ZEPHYR_OK) {
        return throw_err(env, zephyr_last_error(h->db));
    }
    napi_value out{};
    napi_get_boolean(env, true, &out);
    return out;
}

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"open", nullptr, napi_open, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"close", nullptr, napi_close, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exec", nullptr, napi_exec_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"query", nullptr, napi_query_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepare", nullptr, napi_prepare_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stmtQuery", nullptr, napi_stmt_query_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stmtExec", nullptr, napi_stmt_exec_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"aggregateCount", nullptr, napi_aggregate_count_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"aggregateSum", nullptr, napi_aggregate_sum_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"checkpoint", nullptr, napi_checkpoint_fn, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
