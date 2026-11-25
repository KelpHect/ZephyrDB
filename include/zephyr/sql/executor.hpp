#ifndef ZEPHYR_SQL_EXECUTOR_HPP
#define ZEPHYR_SQL_EXECUTOR_HPP

#include "zephyr/c_abi.h"
#include "zephyr/core/arena.hpp"
#include "zephyr/core/augmented_btree.hpp"
#include "zephyr/core/rcu.hpp"
#include "zephyr/sql/ast.hpp"
#include "zephyr/storage/mmap_store.hpp"
#include "zephyr/storage/wal.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace zephyr::sql {

struct Value {
    ColumnType type{ColumnType::Null};
    int64_t i64{0};
    double f64{0.0};
    bool boolean{false};
    std::string str;

    static Value make_null() { return Value{}; }
    static Value make_i64(int64_t v) { Value x; x.type = ColumnType::I64; x.i64 = v; return x; }
    static Value make_f64(double v) { Value x; x.type = ColumnType::F64; x.f64 = v; return x; }
    static Value make_bool(bool v) { Value x; x.type = ColumnType::Bool; x.boolean = v; return x; }
    static Value make_text(std::string v) { Value x; x.type = ColumnType::Text; x.str = std::move(v); return x; }
    static Value make_bytea(std::string v) { Value x; x.type = ColumnType::Bytea; x.str = std::move(v); return x; }

    bool is_null() const noexcept { return type == ColumnType::Null; }
};

struct ColumnSchema {
    std::string name;
    ColumnType type{ColumnType::I64};
    bool primary_key{false};
    bool not_null{false};
    int agg_slot{-1}; // 0..7 in AugmentedBTree if numeric, else -1
};

struct ColumnData {
    ColumnType type{ColumnType::I64};
    std::vector<int64_t> i64_data;
    std::vector<double> f64_data;
    std::vector<uint8_t> bool_data;
    std::vector<std::string> str_data;
    std::vector<uint8_t> nulls;

    void push_value(const Value& v);
    void set_value(size_t row_idx, const Value& v);
    Value get_value(size_t row_idx) const;
    void reserve(size_t n);
};

struct Table {
    std::string name;
    std::vector<ColumnSchema> schema;
    std::vector<ColumnData> columns;
    std::vector<uint8_t> live;
    std::unordered_map<std::string, size_t> col_map;

    int pk_col{-1};
    bool pk_is_i64{false};
    uint64_t next_row_id{1};

    std::unordered_map<int64_t, size_t> pk_i64_index;
    std::unordered_map<std::string, size_t> pk_str_index;
    std::unique_ptr<zephyr::core::AugmentedBTree> btree;

    std::unique_ptr<Table> clone_data(zephyr::core::RcuReaper* reaper) const;
    int find_col(std::string_view col_name) const;
    int find_col_nocase(std::string_view col_name) const noexcept;
};

struct CatalogSnapshot {
    std::vector<std::pair<std::string, Table*>> tables;

    Table* find(std::string_view lower_name) const noexcept;
};

struct CompiledO1Plan {
    struct Item {
        bool is_count{false};
        std::string col_lower;
        std::string label;
    };
    std::string table_lower;
    std::vector<Item> items;
};

struct PreparedStmt {
    std::string sql;
    std::vector<Stmt> stmts;
    std::optional<CompiledO1Plan> o1_plan;
};

class ZephyrEngine {
public:
    using WalReplicateCallback = std::function<void(uint64_t lsn, std::string_view sql)>;

    explicit ZephyrEngine(const ZephyrConfig& cfg);
    ~ZephyrEngine();

    ZephyrEngine(const ZephyrEngine&) = delete;
    ZephyrEngine& operator=(const ZephyrEngine&) = delete;

    int32_t execute(std::string_view sql, uint64_t* rows_affected = nullptr);
    ZephyrResultSet* query(std::string_view sql,
                           const ZephyrValue* params = nullptr,
                           uint32_t param_count = 0);

    std::unique_ptr<PreparedStmt> prepare(std::string_view sql);
    ZephyrResultSet* query_prepared(const PreparedStmt& stmt,
                                    const ZephyrValue* params = nullptr,
                                    uint32_t param_count = 0);
    int32_t exec_prepared(const PreparedStmt& stmt,
                          const ZephyrValue* params = nullptr,
                          uint32_t param_count = 0,
                          uint64_t* rows_affected = nullptr);

    ZephyrAggregate aggregate_count(std::string_view table_name);
    ZephyrAggregate aggregate_sum(std::string_view table_name, std::string_view col_name);

    int32_t checkpoint();
    const char* last_error() const noexcept {
        std::lock_guard<std::mutex> lk(err_mu_);
        return last_error_.c_str();
    }
    const char* result_to_json(const ZephyrResultSet* result, uint32_t* out_len);

    ZephyrResultSet* execute_sql_for_wire(std::string_view sql) { return query(sql); }

    void set_wal_replicate_callback(WalReplicateCallback cb);
    void apply_replicated_wal(uint64_t lsn, std::string_view sql);

    static void free_result(ZephyrResultSet* result);

private:
    struct EvalContext {
        const std::vector<std::string>* col_names{nullptr};
        const std::vector<std::string>* col_qualifiers{nullptr};
        const std::vector<Value>* row{nullptr};
        const std::vector<Value>* params{nullptr};
    };

    ZephyrResultSet* execute_stmts(
        std::string_view raw_sql,
        const std::vector<Stmt>& stmts,
        const ZephyrValue* params,
        uint32_t param_count
    );

    void set_error(std::string_view msg);
    void clear_error();
    ZephyrResultSet* make_error_result(uint32_t status, const std::string& msg);
    ZephyrResultSet* pack_result(
        const std::vector<std::string>& col_names,
        const std::vector<ColumnType>& col_types,
        const std::vector<std::vector<Value>>& rows,
        uint64_t rows_affected,
        uint32_t flags
    );

    Value eval_expr(const Expr& expr, const EvalContext& ctx);
    Value coerce_value(const Value& v, ColumnType target, std::string& err);
    bool truthy(const Value& v) const noexcept;
    int compare_values(const Value& a, const Value& b) const noexcept;

    ZephyrResultSet* exec_stmt(const Stmt& stmt,
                               std::string_view raw_sql,
                               const std::vector<Value>& params,
                               bool log_wal);
    ZephyrResultSet* exec_create_table(const CreateTableStmt& s);
    ZephyrResultSet* exec_drop_table(const DropTableStmt& s);
    ZephyrResultSet* exec_insert(const InsertStmt& s, const std::vector<Value>& params);
    ZephyrResultSet* exec_update(const UpdateStmt& s, const std::vector<Value>& params);
    ZephyrResultSet* exec_delete(const DeleteStmt& s, const std::vector<Value>& params);
    ZephyrResultSet* exec_select(
        const SelectStmt& s,
        const std::vector<Value>& params,
        const std::unordered_map<std::string, std::unique_ptr<Table>>* cte_scope = nullptr
    );
    ZephyrResultSet* exec_txn(const TxnStmt& s);

    Table* resolve_table(
        std::string_view name,
        const std::unordered_map<std::string, std::unique_ptr<Table>>* cte_scope
    );

    std::vector<uint32_t> scan_matching_rows(
        const Table& tbl,
        const Expr* where,
        const std::vector<Value>& params
    );

    void rebuild_btree(Table& tbl);
    void update_btree_for_row(Table& tbl, size_t row_idx);
    void publish_catalog_snapshot();

    void log_wal_entry(std::string_view sql);
    bool load_snapshot(const std::string& snap_path);
    std::vector<uint8_t> serialize_snapshot() const;

    ZephyrConfig config_{};
    std::string data_dir_;
    std::string snap_path_;
    std::string wal_path_;

    mutable std::shared_mutex mu_;
    mutable std::mutex err_mu_;
    zephyr::core::RcuReaper reaper_;
    zephyr::storage::WalManager wal_;
    zephyr::storage::MmapStore mmap_store_;
    zephyr::storage::Compactor compactor_;

    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    std::atomic<CatalogSnapshot*> catalog_snap_{nullptr};

    std::atomic<bool> in_txn_{false};
    bool in_replay_{false};
    std::unordered_map<std::string, std::unique_ptr<Table>> txn_backup_;
    std::vector<std::string> txn_wal_buffer_;

    WalReplicateCallback replicate_cb_;
    std::atomic<bool> has_error_{false};
    std::string last_error_;
    std::string last_json_;
};

} // namespace zephyr::sql

#endif // ZEPHYR_SQL_EXECUTOR_HPP
