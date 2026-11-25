#ifndef ZEPHYR_C_ABI_H
#define ZEPHYR_C_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(ZEPHYR_BUILD_DLL)
    #define ZEPHYR_API __declspec(dllexport)
  #elif defined(ZEPHYR_STATIC)
    #define ZEPHYR_API
  #else
    #define ZEPHYR_API __declspec(dllimport)
  #endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__EMSCRIPTEN__)
  #define ZEPHYR_API __attribute__((visibility("default")))
#else
  #define ZEPHYR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZephyrDB ZephyrDB;
typedef struct ZephyrStmt ZephyrStmt;

typedef enum ZephyrStatus {
    ZEPHYR_OK = 0,
    ZEPHYR_ERR_INVALID_ARG = 1,
    ZEPHYR_ERR_SYNTAX = 2,
    ZEPHYR_ERR_NOT_FOUND = 3,
    ZEPHYR_ERR_CONSTRAINT = 4,
    ZEPHYR_ERR_IO = 5,
    ZEPHYR_ERR_INTERNAL = 6,
    ZEPHYR_ERR_BUSY = 7,
    ZEPHYR_ERR_AUTH = 8
} ZephyrStatus;

typedef enum ZephyrType {
    ZEPHYR_TYPE_NULL = 0,
    ZEPHYR_TYPE_I64 = 1,
    ZEPHYR_TYPE_F64 = 2,
    ZEPHYR_TYPE_BOOL = 3,
    ZEPHYR_TYPE_TEXT = 4,
    ZEPHYR_TYPE_BYTEA = 5
} ZephyrType;

enum ZephyrResultFlags {
    ZEPHYR_FLAG_O1_FASTPATH = 1u << 0,
    ZEPHYR_FLAG_RETURNING   = 1u << 1
};

/* 16 bytes on 64-bit platforms (ptr @ 0, len @ 8, reserved @ 12) */
typedef struct ZephyrSlice {
    const uint8_t* ptr;
    uint32_t len;
    uint32_t reserved;
} ZephyrSlice;

/* 24 bytes on 64-bit platforms (type @ 0, _pad @ 4, as @ 8) */
typedef struct ZephyrValue {
    int32_t type;
    uint32_t _pad;
    union {
        int64_t i64;
        double f64;
        uint8_t boolean;
        ZephyrSlice slice;
    } as;
} ZephyrValue;

/* 16 bytes on 64-bit platforms (name @ 0, name_len @ 8, type @ 12) */
typedef struct ZephyrColumnDesc {
    const char* name;
    uint32_t name_len;
    int32_t type;
} ZephyrColumnDesc;

/* 56 bytes on 64-bit platforms */
typedef struct ZephyrResultSet {
    uint64_t row_count;
    uint64_t col_count;
    uint64_t rows_affected;
    const ZephyrColumnDesc* columns;
    const ZephyrValue* values;
    void* _arena_handle;
    uint32_t status;
    uint32_t flags;
} ZephyrResultSet;

/* 32 bytes */
typedef struct ZephyrAggregate {
    int64_t count;
    int64_t sum_i64;
    double sum_f64;
    int32_t status;
    uint32_t _pad;
} ZephyrAggregate;

/* 32 bytes on 64-bit platforms */
typedef struct ZephyrConfig {
    const char* data_dir;
    uint64_t arena_slab_bytes;
    uint32_t wal_flush_ms;
    uint32_t enable_wal;
    uint32_t enable_compactor;
    uint32_t _pad;
} ZephyrConfig;

/* 64 bytes */
typedef struct ZephyrStats {
    uint64_t table_count;
    uint64_t total_rows;
    uint64_t wal_lsn;
    uint64_t wal_flushed_lsn;
    uint64_t rcu_reclaimed;
    uint64_t o1_fastpath_hits;
    uint64_t prepared_queries;
    uint64_t active_txns;
} ZephyrStats;

ZEPHYR_API ZephyrConfig zephyr_default_config(void);
ZEPHYR_API ZephyrDB* zephyr_db_open(const ZephyrConfig* config);
ZEPHYR_API ZephyrDB* zephyr_db_open_encrypted(const ZephyrConfig* config, const uint8_t* key, uint32_t key_len);
ZEPHYR_API void zephyr_db_close(ZephyrDB* db);

ZEPHYR_API int32_t zephyr_exec(ZephyrDB* db, const char* sql, uint64_t* rows_affected);
ZEPHYR_API ZephyrResultSet* zephyr_query(ZephyrDB* db, const char* sql);
ZEPHYR_API ZephyrResultSet* zephyr_query_params(
    ZephyrDB* db,
    const char* sql,
    const ZephyrValue* params,
    uint32_t param_count
);
ZEPHYR_API void zephyr_result_free(ZephyrResultSet* result);

/* Prepared Statements (skip lex/parse overhead on repeated queries) */
ZEPHYR_API ZephyrStmt* zephyr_prepare(ZephyrDB* db, const char* sql);
ZEPHYR_API ZephyrResultSet* zephyr_stmt_query(
    ZephyrStmt* stmt,
    const ZephyrValue* params,
    uint32_t param_count
);
ZEPHYR_API int32_t zephyr_stmt_exec(
    ZephyrStmt* stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    uint64_t* rows_affected
);
ZEPHYR_API void zephyr_stmt_free(ZephyrStmt* stmt);

/* O(1) Augmented B-Tree direct probes */
ZEPHYR_API ZephyrAggregate zephyr_aggregate_count(ZephyrDB* db, const char* table_name);
ZEPHYR_API ZephyrAggregate zephyr_aggregate_sum(ZephyrDB* db, const char* table_name, const char* col_name);

ZEPHYR_API int32_t zephyr_checkpoint(ZephyrDB* db);
ZEPHYR_API int32_t zephyr_exec_sync(ZephyrDB* db, const char* sql, uint64_t* rows_affected);
ZEPHYR_API ZephyrStats zephyr_get_stats(ZephyrDB* db);
ZEPHYR_API const char* zephyr_last_error(ZephyrDB* db);
ZEPHYR_API const char* zephyr_result_to_json(ZephyrDB* db, const ZephyrResultSet* result, uint32_t* out_len);

/* MMORPG Distributed & Wire Protocol APIs */
ZEPHYR_API int32_t zephyr_raft_start(ZephyrDB* db, uint32_t node_id, uint16_t port, const char* peers_csv);
ZEPHYR_API void zephyr_raft_stop(ZephyrDB* db);
ZEPHYR_API int32_t zephyr_pgwire_start(ZephyrDB* db, uint16_t port);
ZEPHYR_API void zephyr_pgwire_stop(ZephyrDB* db);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_C_ABI_H */
