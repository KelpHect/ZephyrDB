#include "zephyr/c_abi.h"

#include "zephyr/net/pgwire.hpp"
#include "zephyr/net/raft.hpp"
#include "zephyr/sql/executor.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// ============================================================================
// C-ABI Memory Layout Verification
// ============================================================================

#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(ZephyrSlice) == 16, "ZephyrSlice must be 16 bytes on 64-bit");
static_assert(alignof(ZephyrSlice) == 8, "ZephyrSlice align must be 8");
static_assert(offsetof(ZephyrSlice, ptr) == 0, "ZephyrSlice.ptr offset");
static_assert(offsetof(ZephyrSlice, len) == 8, "ZephyrSlice.len offset");
static_assert(offsetof(ZephyrSlice, reserved) == 12, "ZephyrSlice.reserved offset");

static_assert(sizeof(ZephyrValue) == 24, "ZephyrValue must be 24 bytes on 64-bit");
static_assert(alignof(ZephyrValue) == 8, "ZephyrValue align must be 8");
static_assert(offsetof(ZephyrValue, type) == 0, "ZephyrValue.type offset");
static_assert(offsetof(ZephyrValue, as) == 8, "ZephyrValue.as offset");

static_assert(sizeof(ZephyrColumnDesc) == 16, "ZephyrColumnDesc must be 16 bytes on 64-bit");
static_assert(alignof(ZephyrColumnDesc) == 8, "ZephyrColumnDesc align must be 8");
static_assert(offsetof(ZephyrColumnDesc, name) == 0, "ZephyrColumnDesc.name offset");
static_assert(offsetof(ZephyrColumnDesc, name_len) == 8, "ZephyrColumnDesc.name_len offset");
static_assert(offsetof(ZephyrColumnDesc, type) == 12, "ZephyrColumnDesc.type offset");

static_assert(sizeof(ZephyrResultSet) == 56, "ZephyrResultSet must be 56 bytes on 64-bit");
static_assert(alignof(ZephyrResultSet) == 8, "ZephyrResultSet align must be 8");
static_assert(offsetof(ZephyrResultSet, row_count) == 0, "ZephyrResultSet.row_count offset");
static_assert(offsetof(ZephyrResultSet, col_count) == 8, "ZephyrResultSet.col_count offset");
static_assert(offsetof(ZephyrResultSet, rows_affected) == 16, "ZephyrResultSet.rows_affected offset");
static_assert(offsetof(ZephyrResultSet, columns) == 24, "ZephyrResultSet.columns offset");
static_assert(offsetof(ZephyrResultSet, values) == 32, "ZephyrResultSet.values offset");
static_assert(offsetof(ZephyrResultSet, _arena_handle) == 40, "ZephyrResultSet._arena_handle offset");
static_assert(offsetof(ZephyrResultSet, status) == 48, "ZephyrResultSet.status offset");
static_assert(offsetof(ZephyrResultSet, flags) == 52, "ZephyrResultSet.flags offset");

static_assert(sizeof(ZephyrAggregate) == 32, "ZephyrAggregate must be 32 bytes");
static_assert(alignof(ZephyrAggregate) == 8, "ZephyrAggregate align must be 8");
static_assert(offsetof(ZephyrAggregate, count) == 0, "ZephyrAggregate.count offset");
static_assert(offsetof(ZephyrAggregate, sum_i64) == 8, "ZephyrAggregate.sum_i64 offset");
static_assert(offsetof(ZephyrAggregate, sum_f64) == 16, "ZephyrAggregate.sum_f64 offset");
static_assert(offsetof(ZephyrAggregate, status) == 24, "ZephyrAggregate.status offset");

static_assert(sizeof(ZephyrConfig) == 32, "ZephyrConfig must be 32 bytes on 64-bit");
static_assert(alignof(ZephyrConfig) == 8, "ZephyrConfig align must be 8");
static_assert(offsetof(ZephyrConfig, data_dir) == 0, "ZephyrConfig.data_dir offset");
static_assert(offsetof(ZephyrConfig, arena_slab_bytes) == 8, "ZephyrConfig.arena_slab_bytes offset");
static_assert(offsetof(ZephyrConfig, wal_flush_ms) == 16, "ZephyrConfig.wal_flush_ms offset");
static_assert(offsetof(ZephyrConfig, enable_wal) == 20, "ZephyrConfig.enable_wal offset");
static_assert(offsetof(ZephyrConfig, enable_compactor) == 24, "ZephyrConfig.enable_compactor offset");
#endif

struct ZephyrDB {
    std::unique_ptr<zephyr::sql::ZephyrEngine> engine;
    std::unique_ptr<zephyr::net::RaftNode> raft;
    std::unique_ptr<zephyr::net::PgWireServer> pgwire;
};

extern "C" {

ZephyrConfig zephyr_default_config(void) {
    ZephyrConfig cfg{};
    cfg.data_dir = nullptr;
    cfg.arena_slab_bytes = 65536;
    cfg.wal_flush_ms = 2;
    cfg.enable_wal = 1;
    cfg.enable_compactor = 1;
    cfg._pad = 0;
    return cfg;
}

ZephyrDB* zephyr_db_open(const ZephyrConfig* config) {
    ZephyrConfig cfg = config ? *config : zephyr_default_config();
    auto* db = new ZephyrDB();
    db->engine = std::make_unique<zephyr::sql::ZephyrEngine>(cfg);
    return db;
}

void zephyr_db_close(ZephyrDB* db) {
    if (!db) return;
    if (db->pgwire) {
        db->pgwire->stop();
        db->pgwire.reset();
    }
    if (db->raft) {
        db->raft->stop();
        db->raft.reset();
    }
    db->engine.reset();
    delete db;
}

int32_t zephyr_exec(ZephyrDB* db, const char* sql, uint64_t* rows_affected) {
    if (!db || !db->engine || !sql) {
        return ZEPHYR_ERR_INVALID_ARG;
    }
    return db->engine->execute(sql, rows_affected);
}

ZephyrResultSet* zephyr_query(ZephyrDB* db, const char* sql) {
    return zephyr_query_params(db, sql, nullptr, 0);
}

ZephyrResultSet* zephyr_query_params(
    ZephyrDB* db,
    const char* sql,
    const ZephyrValue* params,
    uint32_t param_count
) {
    if (!db || !db->engine || !sql) {
        return nullptr;
    }
    return db->engine->query(sql, params, param_count);
}

void zephyr_result_free(ZephyrResultSet* result) {
    zephyr::sql::ZephyrEngine::free_result(result);
}

ZephyrAggregate zephyr_aggregate_count(ZephyrDB* db, const char* table_name) {
    if (!db || !db->engine || !table_name) {
        ZephyrAggregate agg{};
        agg.status = ZEPHYR_ERR_INVALID_ARG;
        return agg;
    }
    return db->engine->aggregate_count(table_name);
}

ZephyrAggregate zephyr_aggregate_sum(ZephyrDB* db, const char* table_name, const char* col_name) {
    if (!db || !db->engine || !table_name || !col_name) {
        ZephyrAggregate agg{};
        agg.status = ZEPHYR_ERR_INVALID_ARG;
        return agg;
    }
    return db->engine->aggregate_sum(table_name, col_name);
}

int32_t zephyr_checkpoint(ZephyrDB* db) {
    if (!db || !db->engine) {
        return ZEPHYR_ERR_INVALID_ARG;
    }
    return db->engine->checkpoint();
}

const char* zephyr_last_error(ZephyrDB* db) {
    if (!db || !db->engine) {
        return "invalid database handle";
    }
    return db->engine->last_error();
}

const char* zephyr_result_to_json(ZephyrDB* db, const ZephyrResultSet* result, uint32_t* out_len) {
    if (!db || !db->engine) {
        if (out_len) *out_len = 0;
        return "";
    }
    return db->engine->result_to_json(result, out_len);
}

int32_t zephyr_raft_start(ZephyrDB* db, uint32_t node_id, uint16_t port, const char* peers_csv) {
    if (!db || !db->engine) {
        return ZEPHYR_ERR_INVALID_ARG;
    }
    if (!db->raft) {
        db->raft = std::make_unique<zephyr::net::RaftNode>();
    } else if (db->raft->is_running()) {
        db->raft->stop();
    }

    std::string_view peers = peers_csv ? std::string_view(peers_csv) : std::string_view{};
    bool ok = db->raft->start(
        node_id,
        port,
        peers,
        [db](uint64_t lsn, std::string_view sql) {
            if (db && db->engine) {
                db->engine->apply_replicated_wal(lsn, sql);
            }
        }
    );
    if (!ok) {
        return ZEPHYR_ERR_IO;
    }

    db->engine->set_wal_replicate_callback([db](uint64_t lsn, std::string_view sql) {
        if (db && db->raft && db->raft->is_running() &&
            db->raft->state() == zephyr::net::RaftState::Leader) {
            db->raft->propose_wal_entry(lsn, sql);
        }
    });
    return ZEPHYR_OK;
}

void zephyr_raft_stop(ZephyrDB* db) {
    if (!db) return;
    if (db->engine) {
        db->engine->set_wal_replicate_callback({});
    }
    if (db->raft) {
        db->raft->stop();
        db->raft.reset();
    }
}

int32_t zephyr_pgwire_start(ZephyrDB* db, uint16_t port) {
    if (!db || !db->engine) {
        return ZEPHYR_ERR_INVALID_ARG;
    }
    if (!db->pgwire) {
        db->pgwire = std::make_unique<zephyr::net::PgWireServer>();
    } else if (db->pgwire->is_running()) {
        db->pgwire->stop();
    }
    if (!db->pgwire->start(db, port)) {
        return ZEPHYR_ERR_IO;
    }
    return ZEPHYR_OK;
}

void zephyr_pgwire_stop(ZephyrDB* db) {
    if (!db || !db->pgwire) return;
    db->pgwire->stop();
    db->pgwire.reset();
}

} // extern "C"
