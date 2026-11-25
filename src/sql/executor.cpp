#include "zephyr/sql/executor.hpp"

#include "zephyr/sql/parser.hpp"
#include "zephyr/sql/reflection.hpp"
#include "zephyr/sql/simd_eval.hpp"
#include "zephyr/storage/crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>

namespace zephyr::sql {

namespace {

// ============================================================================
// Thread-Local + Global Fallback Arena Slab Pool
// ============================================================================

class GlobalArenaPool {
public:
    static constexpr size_t MAX_GLOBAL = 64;

    GlobalArenaPool() = default;
    ~GlobalArenaPool() {
        alive_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lk(mu_);
        for (auto* a : pool_) {
            delete a;
        }
        pool_.clear();
    }

    GlobalArenaPool(const GlobalArenaPool&) = delete;
    GlobalArenaPool& operator=(const GlobalArenaPool&) = delete;

    zephyr::core::Arena* try_pop() {
        if (!alive_.load(std::memory_order_acquire)) return nullptr;
        std::lock_guard<std::mutex> lk(mu_);
        if (pool_.empty()) return nullptr;
        auto* a = pool_.back();
        pool_.pop_back();
        return a;
    }

    bool try_push(zephyr::core::Arena* a) {
        if (!alive_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lk(mu_);
        if (pool_.size() >= MAX_GLOBAL) return false;
        pool_.push_back(a);
        return true;
    }

private:
    std::atomic<bool> alive_{true};
    std::mutex mu_;
    std::vector<zephyr::core::Arena*> pool_;
};

GlobalArenaPool& global_arena_pool() {
    static GlobalArenaPool pool;
    return pool;
}

struct ThreadArenaCache {
    static constexpr size_t MAX_LOCAL = 8;

    zephyr::core::Arena* slots[MAX_LOCAL]{};
    size_t count{0};

    ~ThreadArenaCache() {
        for (size_t i = 0; i < count; ++i) {
            delete slots[i];
        }
        count = 0;
    }
};

thread_local ThreadArenaCache tls_arena_cache;

zephyr::core::Arena* acquire_arena(size_t slab_bytes) {
    if (tls_arena_cache.count > 0) {
        return tls_arena_cache.slots[--tls_arena_cache.count];
    }
    if (auto* a = global_arena_pool().try_pop()) {
        return a;
    }
    return new zephyr::core::Arena(slab_bytes);
}

void release_arena(zephyr::core::Arena* arena) noexcept {
    if (!arena) return;
    if (arena->slab_count() > 4) {
        delete arena;
        return;
    }
    arena->reset();
    if (tls_arena_cache.count < ThreadArenaCache::MAX_LOCAL) {
        tls_arena_cache.slots[tls_arena_cache.count++] = arena;
        return;
    }
    if (!global_arena_pool().try_push(arena)) {
        delete arena;
    }
}

std::string to_lower(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view to_lower_sv(std::string_view sv, char* buf, size_t buf_cap, std::string& heap_fallback) {
    if (sv.size() <= buf_cap) {
        for (size_t i = 0; i < sv.size(); ++i) {
            buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(sv[i])));
        }
        return std::string_view(buf, sv.size());
    }
    heap_fallback = to_lower(sv);
    return heap_fallback;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

Value from_c_value(const ZephyrValue& zv) {
    switch (zv.type) {
        case ZEPHYR_TYPE_I64: return Value::make_i64(zv.as.i64);
        case ZEPHYR_TYPE_F64: return Value::make_f64(zv.as.f64);
        case ZEPHYR_TYPE_BOOL: return Value::make_bool(zv.as.boolean != 0);
        case ZEPHYR_TYPE_TEXT:
            if (zv.as.slice.ptr && zv.as.slice.len > 0) {
                return Value::make_text(std::string(
                    reinterpret_cast<const char*>(zv.as.slice.ptr), zv.as.slice.len));
            }
            return Value::make_text("");
        case ZEPHYR_TYPE_BYTEA:
            if (zv.as.slice.ptr && zv.as.slice.len > 0) {
                return Value::make_bytea(std::string(
                    reinterpret_cast<const char*>(zv.as.slice.ptr), zv.as.slice.len));
            }
            return Value::make_bytea("");
        case ZEPHYR_TYPE_NULL:
        default:
            return Value::make_null();
    }
}

ZephyrValue to_c_value(const Value& v, zephyr::core::Arena& arena) {
    ZephyrValue zv{};
    switch (v.type) {
        case ColumnType::Null:
            zv.type = ZEPHYR_TYPE_NULL;
            break;
        case ColumnType::I64:
            zv.type = ZEPHYR_TYPE_I64;
            zv.as.i64 = v.i64;
            break;
        case ColumnType::F64:
            zv.type = ZEPHYR_TYPE_F64;
            zv.as.f64 = v.f64;
            break;
        case ColumnType::Bool:
            zv.type = ZEPHYR_TYPE_BOOL;
            zv.as.boolean = v.boolean ? 1 : 0;
            break;
        case ColumnType::Text:
            zv.type = ZEPHYR_TYPE_TEXT;
            zv.as.slice.len = static_cast<uint32_t>(v.str.size());
            zv.as.slice.ptr = v.str.empty()
                ? reinterpret_cast<const uint8_t*>("")
                : arena.dup_bytes(v.str.data(), v.str.size());
            break;
        case ColumnType::Bytea:
            zv.type = ZEPHYR_TYPE_BYTEA;
            zv.as.slice.len = static_cast<uint32_t>(v.str.size());
            zv.as.slice.ptr = v.str.empty()
                ? nullptr
                : arena.dup_bytes(v.str.data(), v.str.size());
            break;
    }
    return zv;
}

bool contains_aggregate(const Expr* e) {
    if (!e) return false;
    return std::visit([](const auto& node) -> bool {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, AggregateExpr>) {
            return true;
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            return contains_aggregate(node.operand.get());
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            return contains_aggregate(node.left.get()) || contains_aggregate(node.right.get());
        } else if constexpr (std::is_same_v<T, IsNullExpr>) {
            return contains_aggregate(node.operand.get());
        } else if constexpr (std::is_same_v<T, BetweenExpr>) {
            return contains_aggregate(node.expr.get()) ||
                   contains_aggregate(node.low.get()) ||
                   contains_aggregate(node.high.get());
        } else {
            return false;
        }
    }, e->node);
}

std::string infer_expr_name(const Expr* e, size_t idx) {
    if (!e) return "col_" + std::to_string(idx);
    return std::visit([&](const auto& node) -> std::string {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, ColumnRefExpr>) {
            return node.column;
        } else if constexpr (std::is_same_v<T, AggregateExpr>) {
            switch (node.func) {
                case AggFunc::Count: return "count";
                case AggFunc::Sum: return "sum";
                case AggFunc::Min: return "min";
                case AggFunc::Max: return "max";
                case AggFunc::Avg: return "avg";
            }
        }
        return "col_" + std::to_string(idx);
    }, e->node);
}

std::optional<CmpOp> to_cmp_op(BinaryOp op, bool flip = false) {
    if (!flip) {
        switch (op) {
            case BinaryOp::Eq: return CmpOp::eq;
            case BinaryOp::Ne: return CmpOp::ne;
            case BinaryOp::Lt: return CmpOp::lt;
            case BinaryOp::Le: return CmpOp::le;
            case BinaryOp::Gt: return CmpOp::gt;
            case BinaryOp::Ge: return CmpOp::ge;
            default: return std::nullopt;
        }
    } else {
        switch (op) {
            case BinaryOp::Eq: return CmpOp::eq;
            case BinaryOp::Ne: return CmpOp::ne;
            case BinaryOp::Lt: return CmpOp::gt;
            case BinaryOp::Le: return CmpOp::ge;
            case BinaryOp::Gt: return CmpOp::lt;
            case BinaryOp::Ge: return CmpOp::le;
            default: return std::nullopt;
        }
    }
}

std::string value_key_string(const Value& v) {
    switch (v.type) {
        case ColumnType::Null: return "\x00";
        case ColumnType::I64: return "\x01" + std::to_string(v.i64);
        case ColumnType::F64: return "\x02" + std::to_string(v.f64);
        case ColumnType::Bool: return v.boolean ? "\x03T" : "\x03F";
        case ColumnType::Text: return "\x04" + v.str;
        case ColumnType::Bytea: return "\x05" + v.str;
    }
    return "";
}

bool is_virtual_system_table(std::string_view lower_name) noexcept {
    return lower_name == "information_schema.tables" ||
           lower_name == "information_schema.columns" ||
           lower_name == "pg_catalog.pg_tables";
}

const char* column_type_sql_name(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::I64: return "BIGINT";
        case ColumnType::F64: return "DOUBLE PRECISION";
        case ColumnType::Bool: return "BOOLEAN";
        case ColumnType::Text: return "TEXT";
        case ColumnType::Bytea: return "BYTEA";
        case ColumnType::Null: return "NULL";
    }
    return "NULL";
}

Value eval_literal_expr(const LiteralExpr& lit) {
    return std::visit([](const auto& val) -> Value {
        using V = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<V, std::nullptr_t>) return Value::make_null();
        else if constexpr (std::is_same_v<V, int64_t>) return Value::make_i64(val);
        else if constexpr (std::is_same_v<V, double>) return Value::make_f64(val);
        else if constexpr (std::is_same_v<V, bool>) return Value::make_bool(val);
        else return Value::make_text(val);
    }, lit.value);
}

std::string format_sql_literal(const Value& v) {
    switch (v.type) {
        case ColumnType::Null: return "NULL";
        case ColumnType::I64: return std::to_string(v.i64);
        case ColumnType::F64: {
            std::ostringstream oss;
            oss.precision(17);
            oss << v.f64;
            return oss.str();
        }
        case ColumnType::Bool: return v.boolean ? "TRUE" : "FALSE";
        case ColumnType::Text:
        case ColumnType::Bytea: {
            std::string out;
            out.reserve(v.str.size() + 2);
            out.push_back('\'');
            for (char c : v.str) {
                if (c == '\'') out.push_back('\'');
                out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }
    }
    return "NULL";
}

// Binary serialization helpers for snapshot
void write_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }
void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void write_str(std::vector<uint8_t>& buf, std::string_view s) {
    write_u32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

bool read_u8(const uint8_t*& p, const uint8_t* end, uint8_t& out) {
    if (p + 1 > end) return false;
    out = *p++;
    return true;
}
bool read_u32(const uint8_t*& p, const uint8_t* end, uint32_t& out) {
    if (p + 4 > end) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(p[i]) << (i * 8);
    p += 4;
    return true;
}
bool read_u64(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    if (p + 8 > end) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(p[i]) << (i * 8);
    p += 8;
    return true;
}
bool read_str(const uint8_t*& p, const uint8_t* end, std::string& out) {
    uint32_t len = 0;
    if (!read_u32(p, end, len)) return false;
    if (p + len > end) return false;
    out.assign(reinterpret_cast<const char*>(p), len);
    p += len;
    return true;
}

struct AllShardsLock {
    const Table& tbl;
    explicit AllShardsLock(const Table& t) : tbl(t) {
        for (size_t i = 0; i < Table::WRITE_SHARDS; ++i) {
            tbl.row_shard_mu[i].lock();
        }
    }
    ~AllShardsLock() {
        for (size_t i = Table::WRITE_SHARDS; i > 0; --i) {
            tbl.row_shard_mu[i - 1].unlock();
        }
    }
    AllShardsLock(const AllShardsLock&) = delete;
    AllShardsLock& operator=(const AllShardsLock&) = delete;
};

void secure_zero(void* ptr, size_t len) noexcept {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
}

} // namespace

// ============================================================================
// ColumnData & Table
// ============================================================================

void ColumnData::push_value(const Value& v) {
    bool is_null = v.is_null();
    nulls.push_back(is_null ? 1 : 0);
    switch (type) {
        case ColumnType::I64:
            i64_data.push_back(is_null ? 0 : v.i64);
            break;
        case ColumnType::F64:
            f64_data.push_back(is_null ? 0.0 : v.f64);
            break;
        case ColumnType::Bool:
            bool_data.push_back(is_null ? 0 : (v.boolean ? 1 : 0));
            break;
        case ColumnType::Text:
        case ColumnType::Bytea:
            str_data.push_back(is_null ? std::string{} : v.str);
            break;
        case ColumnType::Null:
            break;
    }
}

void ColumnData::set_value(size_t row_idx, const Value& v) {
    bool is_null = v.is_null();
    nulls[row_idx] = is_null ? 1 : 0;
    switch (type) {
        case ColumnType::I64:
            i64_data[row_idx] = is_null ? 0 : v.i64;
            break;
        case ColumnType::F64:
            f64_data[row_idx] = is_null ? 0.0 : v.f64;
            break;
        case ColumnType::Bool:
            bool_data[row_idx] = is_null ? 0 : (v.boolean ? 1 : 0);
            break;
        case ColumnType::Text:
        case ColumnType::Bytea:
            str_data[row_idx] = is_null ? std::string{} : v.str;
            break;
        case ColumnType::Null:
            break;
    }
}

Value ColumnData::get_value(size_t row_idx) const {
    if (nulls[row_idx]) return Value::make_null();
    switch (type) {
        case ColumnType::I64: return Value::make_i64(i64_data[row_idx]);
        case ColumnType::F64: return Value::make_f64(f64_data[row_idx]);
        case ColumnType::Bool: return Value::make_bool(bool_data[row_idx] != 0);
        case ColumnType::Text: return Value::make_text(str_data[row_idx]);
        case ColumnType::Bytea: return Value::make_bytea(str_data[row_idx]);
        case ColumnType::Null: return Value::make_null();
    }
    return Value::make_null();
}

void ColumnData::reserve(size_t n) {
    nulls.reserve(n);
    switch (type) {
        case ColumnType::I64: i64_data.reserve(n); break;
        case ColumnType::F64: f64_data.reserve(n); break;
        case ColumnType::Bool: bool_data.reserve(n); break;
        case ColumnType::Text:
        case ColumnType::Bytea: str_data.reserve(n); break;
        case ColumnType::Null: break;
    }
}

int Table::find_col(std::string_view col_name) const {
    auto it = col_map.find(to_lower(col_name));
    if (it == col_map.end()) return -1;
    return static_cast<int>(it->second);
}

int Table::find_col_nocase(std::string_view col_name) const noexcept {
    for (size_t i = 0; i < schema.size(); ++i) {
        if (iequals_ascii(schema[i].name, col_name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Table* CatalogSnapshot::find(std::string_view lower_name) const noexcept {
    const size_t n = tables.size();
    if (n <= 8) {
        for (size_t i = 0; i < n; ++i) {
            if (tables[i].first == lower_name) {
                return tables[i].second;
            }
        }
        return nullptr;
    }
    auto it = std::lower_bound(
        tables.begin(), tables.end(), lower_name,
        [](const std::pair<std::string, Table*>& entry, std::string_view key) {
            return std::string_view(entry.first) < key;
        });
    if (it != tables.end() && std::string_view(it->first) == lower_name) {
        return it->second;
    }
    return nullptr;
}

std::unique_ptr<Table> Table::clone_data(zephyr::core::RcuReaper* reaper) const {
    auto copy = std::make_unique<Table>();
    copy->name = name;
    copy->schema = schema;
    copy->columns = columns;
    copy->live = live;
    copy->col_map = col_map;
    copy->pk_col = pk_col;
    copy->pk_is_i64 = pk_is_i64;
    copy->next_row_id = next_row_id;
    copy->pk_shard_index = pk_shard_index;
    copy->pk_str_index = pk_str_index;
    copy->btree = std::make_unique<zephyr::core::ShardedAugmentedBTree>(reaper);
    return copy;
}

// ============================================================================
// ZephyrEngine Lifecycle & Persistence
// ============================================================================

ZephyrEngine::ZephyrEngine(const ZephyrConfig& cfg)
    : ZephyrEngine(cfg, nullptr, 0) {}

ZephyrEngine::ZephyrEngine(const ZephyrConfig& cfg, const uint8_t* enc_key, uint32_t enc_key_len)
    : config_(cfg) {
    if (config_.arena_slab_bytes == 0) {
        config_.arena_slab_bytes = 65536;
    }
    if (enc_key && enc_key_len > 0) {
        encrypted_ = true;
        zephyr::storage::crypto::hkdf_derive_keys(enc_key, enc_key_len, wal_key_, snap_key_);
        wal_.set_encryption_key(wal_key_, sizeof(wal_key_));
    }

    reaper_.start(2);
    publish_catalog_snapshot();

    if (cfg.data_dir && cfg.data_dir[0] != '\0') {
        data_dir_ = cfg.data_dir;
        std::error_code ec;
        std::filesystem::create_directories(data_dir_, ec);

        snap_path_ = (std::filesystem::path(data_dir_) / "zephyr.snap").string();
        wal_path_ = (std::filesystem::path(data_dir_) / "zephyr.wal").string();

        if (std::filesystem::exists(snap_path_, ec)) {
            load_snapshot(snap_path_);
        }

        if (auth_ok_ && cfg.enable_wal) {
            uint32_t flush_ms = cfg.wal_flush_ms > 0 ? cfg.wal_flush_ms : 2;
            wal_.open(wal_path_, flush_ms);
            bool wal_auth_failed = false;
            auto records = wal_.replay(&wal_auth_failed);
            if (wal_auth_failed) {
                auth_ok_ = false;
                set_error("WAL authentication failed");
                wal_.close();
            } else if (!records.empty()) {
                in_replay_ = true;
                for (const auto& rec : records) {
                    execute(rec.sql, nullptr);
                }
                in_replay_ = false;
            }
        }

        if (auth_ok_ && cfg.enable_compactor) {
            compactor_.start(5000, [this]() { checkpoint(); });
        }
    }
}

ZephyrEngine::~ZephyrEngine() {
    compactor_.stop();
    if (wal_.is_open()) {
        wal_.flush();
        wal_.close();
    }
    mmap_store_.close_mmap();
    CatalogSnapshot* snap = catalog_snap_.exchange(nullptr, std::memory_order_acq_rel);
    delete snap;
    StmtCacheSnapshot* sc_snap = stmt_cache_snap_.exchange(nullptr, std::memory_order_acq_rel);
    delete sc_snap;
    tables_.clear();
    txn_backup_.clear();
    reaper_.stop();
    secure_zero(wal_key_, sizeof(wal_key_));
    secure_zero(snap_key_, sizeof(snap_key_));
}

uint32_t ZephyrEngine::stat_slot() noexcept {
    thread_local uint32_t tl_slot = UINT32_MAX;
    if (tl_slot == UINT32_MAX) {
        tl_slot = next_stat_slot_.fetch_add(1, std::memory_order_relaxed) & 63u;
    }
    return tl_slot;
}

void ZephyrEngine::publish_catalog_snapshot() {
    auto* next = new CatalogSnapshot();
    next->tables.reserve(tables_.size());
    for (const auto& [k, tbl] : tables_) {
        next->tables.emplace_back(k, tbl.get());
    }
    std::sort(next->tables.begin(), next->tables.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    CatalogSnapshot* old = catalog_snap_.exchange(next, std::memory_order_acq_rel);
    if (old) {
        reaper_.retire_obj(old);
    }
}

void ZephyrEngine::free_result(ZephyrResultSet* result) {
    if (!result) return;
    auto* arena = static_cast<zephyr::core::Arena*>(result->_arena_handle);
    if (arena) {
        release_arena(arena);
    }
}

void ZephyrEngine::set_wal_replicate_callback(WalReplicateCallback cb) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    replicate_cb_ = std::move(cb);
}

void ZephyrEngine::apply_replicated_wal(uint64_t /*lsn*/, std::string_view sql) {
    std::unique_lock<std::shared_mutex> lk(mu_);
    in_replay_ = true;
    auto parsed = parse_sql(sql);
    if (parsed.ok()) {
        for (const auto& stmt : parsed.stmts) {
            std::optional<AllShardsLock> shards_lk;
            std::string_view tname;
            if (const auto* ins = std::get_if<InsertStmt>(&stmt)) tname = ins->table_name;
            else if (const auto* upd = std::get_if<UpdateStmt>(&stmt)) tname = upd->table_name;
            else if (const auto* del = std::get_if<DeleteStmt>(&stmt)) tname = del->table_name;
            if (!tname.empty()) {
                auto it = tables_.find(to_lower(tname));
                if (it != tables_.end() && it->second) {
                    shards_lk.emplace(*it->second);
                }
            }
            auto* res = exec_stmt(stmt, sql, {}, false);
            free_result(res);
        }
    }
    in_replay_ = false;
}

// ============================================================================
// Public Query & Execution APIs
// ============================================================================

int32_t ZephyrEngine::execute(std::string_view sql, uint64_t* rows_affected) {
    ZephyrResultSet* res = query(sql, nullptr, 0);
    if (!res) return ZEPHYR_ERR_INTERNAL;
    int32_t status = static_cast<int32_t>(res->status);
    if (rows_affected) {
        *rows_affected = res->rows_affected;
    }
    free_result(res);
    return status;
}

int32_t ZephyrEngine::execute_sync(std::string_view sql, uint64_t* rows_affected) {
    clear_error();
    ParseResult parsed = parse_sql(sql);
    if (!parsed.ok()) {
        set_error(parsed.error);
        return ZEPHYR_ERR_SYNTAX;
    }
    ZephyrResultSet* res = execute_stmts(sql, parsed.stmts, nullptr, 0, true);
    if (!res) return ZEPHYR_ERR_INTERNAL;
    int32_t status = static_cast<int32_t>(res->status);
    if (rows_affected) {
        *rows_affected = res->rows_affected;
    }
    free_result(res);
    return status;
}

ZephyrResultSet* ZephyrEngine::query(
    std::string_view sql,
    const ZephyrValue* params,
    uint32_t param_count
) {
    // Fastpath: lock-free RCU lookup in automatic statement cache
    // (zero std::string allocation and zero std::shared_ptr atomic refcount on fastpath hits)
    {
        std::shared_ptr<PreparedStmt> slow_pin;
        {
            cpp26::rcu::scoped_read_lock rcu_guard(reaper_.domain());
            const StmtCacheSnapshot* snap = stmt_cache_snap_.load(std::memory_order_acquire);
            if (snap) {
                auto it = snap->entries.find(sql);
                if (it != snap->entries.end()) {
                    const PreparedStmt* cached_ptr = it->second;
                    if (!in_txn_.load(std::memory_order_acquire) &&
                        (cached_ptr->pk_select_plan.has_value() ||
                         cached_ptr->pk_update_plan.has_value() ||
                         (cached_ptr->o1_plan.has_value() && cached_ptr->o1_plan->items.size() == 1))) {
                        return query_prepared_impl(*cached_ptr, params, param_count, /*rcu_held=*/true);
                    }
                    // Slow path or multi-item plan: pin shared_ptr before leaving RCU guard
                    for (const auto& owner : snap->owners) {
                        if (owner.get() == cached_ptr) {
                            slow_pin = owner;
                            break;
                        }
                    }
                }
            }
        }
        if (slow_pin) {
            return query_prepared_impl(*slow_pin, params, param_count, /*rcu_held=*/false);
        }
    }

    // Cache miss: parse and compile via prepare(sql)
    std::unique_ptr<PreparedStmt> ps = prepare(sql);
    if (!ps) {
        return make_error_result(ZEPHYR_ERR_SYNTAX, last_error());
    }

    if (ps->stmts.size() == 1 && !in_replay_) {
        std::shared_ptr<PreparedStmt> sp(std::move(ps));
        {
            std::lock_guard<std::mutex> lk(stmt_cache_mu_);
            StmtCacheSnapshot* old = stmt_cache_snap_.load(std::memory_order_relaxed);
            if (!old || old->entries.find(sp->sql) == old->entries.end()) {
                auto* next = new StmtCacheSnapshot();
                if (old && old->owners.size() < STMT_CACHE_MAX_ENTRIES) {
                    next->owners = old->owners;
                }
                next->owners.push_back(sp);
                next->entries.reserve(next->owners.size());
                for (const auto& owner : next->owners) {
                    next->entries.emplace(std::string_view(owner->sql), owner.get());
                }
                StmtCacheSnapshot* prev = stmt_cache_snap_.exchange(next, std::memory_order_acq_rel);
                if (prev) {
                    reaper_.retire_obj(prev);
                }
            }
        }
        return query_prepared_impl(*sp, params, param_count, /*rcu_held=*/false);
    }

    return execute_stmts(ps->sql, ps->stmts, params, param_count, false);
}

std::unique_ptr<PreparedStmt> ZephyrEngine::prepare(std::string_view sql) {
    clear_error();
    ParseResult parsed = parse_sql(sql);
    if (!parsed.ok()) {
        set_error(parsed.error);
        return nullptr;
    }
    auto ps = std::make_unique<PreparedStmt>();
    ps->sql = std::string(sql);
    ps->stmts = std::move(parsed.stmts);

    // Pre-compile O(1) aggregate fastpath plan for single-table COUNT(*)/SUM(col) SELECTs
    if (ps->stmts.size() == 1 && std::holds_alternative<SelectStmt>(ps->stmts[0])) {
        const auto& s = std::get<SelectStmt>(ps->stmts[0]);
        if (s.ctes.empty() && s.from.has_value() && s.joins.empty() &&
            !s.where && s.group_by.empty() && !s.limit.has_value() &&
            !s.offset.has_value() && !s.items.empty()) {
            CompiledO1Plan plan;
            plan.table_lower = to_lower(s.from->table_name);
            if (!is_virtual_system_table(plan.table_lower)) {
                plan.items.reserve(s.items.size());
                bool all_o1 = true;

                for (size_t i = 0; i < s.items.size(); ++i) {
                    const auto& item = s.items[i];
                    if (item.is_star || !item.expr ||
                        !std::holds_alternative<AggregateExpr>(item.expr->node)) {
                        all_o1 = false;
                        break;
                    }
                    const auto& agg = std::get<AggregateExpr>(item.expr->node);
                    std::string label = item.alias.empty()
                        ? infer_expr_name(item.expr.get(), i) : item.alias;

                    if (agg.func == AggFunc::Count && agg.is_star) {
                        plan.items.push_back(CompiledO1Plan::Item{true, {}, std::move(label)});
                    } else if (agg.func == AggFunc::Sum && agg.arg &&
                               std::holds_alternative<ColumnRefExpr>(agg.arg->node)) {
                        const auto& cref = std::get<ColumnRefExpr>(agg.arg->node);
                        plan.items.push_back(CompiledO1Plan::Item{
                            false, to_lower(cref.column), std::move(label)});
                    } else {
                        all_o1 = false;
                        break;
                    }
                }

                if (all_o1 && !plan.items.empty()) {
                    ps->o1_plan = std::move(plan);
                }
            }
        } else if (s.ctes.empty() && s.from.has_value() && s.joins.empty() &&
                   s.where && s.group_by.empty() && s.order_by.empty() &&
                   !s.distinct && !s.limit.has_value() && !s.offset.has_value() &&
                   !s.items.empty() && std::holds_alternative<BinaryExpr>(s.where->node)) {
            std::string tbl_lower = to_lower(s.from->table_name);
            if (!is_virtual_system_table(tbl_lower)) {
                const auto& wbin = std::get<BinaryExpr>(s.where->node);
                if (wbin.op == BinaryOp::Eq && wbin.left && wbin.right) {
                    const ColumnRefExpr* pk_cref = nullptr;
                    const Expr* pk_rhs = nullptr;
                    if (std::holds_alternative<ColumnRefExpr>(wbin.left->node)) {
                        pk_cref = &std::get<ColumnRefExpr>(wbin.left->node);
                        pk_rhs = wbin.right.get();
                    } else if (std::holds_alternative<ColumnRefExpr>(wbin.right->node)) {
                        pk_cref = &std::get<ColumnRefExpr>(wbin.right->node);
                        pk_rhs = wbin.left.get();
                    }

                    std::string base_qual = s.from->alias.empty() ? s.from->table_name : s.from->alias;
                    bool pk_ok = false;
                    uint32_t pk_param_idx = 0;
                    int64_t pk_literal = 0;
                    if (pk_cref && pk_rhs &&
                        (pk_cref->table.empty() || iequals_ascii(pk_cref->table, base_qual))) {
                        if (std::holds_alternative<ParamExpr>(pk_rhs->node)) {
                            pk_param_idx = std::get<ParamExpr>(pk_rhs->node).index;
                            pk_ok = (pk_param_idx > 0);
                        } else if (std::holds_alternative<LiteralExpr>(pk_rhs->node)) {
                            const auto& lit = std::get<LiteralExpr>(pk_rhs->node);
                            if (std::holds_alternative<int64_t>(lit.value)) {
                                pk_literal = std::get<int64_t>(lit.value);
                                pk_ok = true;
                            }
                        }
                    }

                    if (pk_ok) {
                        CompiledPkSelectPlan plan;
                        plan.table_lower = std::move(tbl_lower);
                        plan.pk_col_lower = to_lower(pk_cref->column);
                        plan.pk_param_idx = pk_param_idx;
                        plan.pk_literal = pk_literal;

                        bool proj_ok = true;
                        if (s.items.size() == 1 && s.items[0].is_star) {
                            if (s.items[0].table_qualifier.empty() ||
                                iequals_ascii(s.items[0].table_qualifier, base_qual)) {
                                plan.select_star = true;
                            } else {
                                proj_ok = false;
                            }
                        } else {
                            plan.items.reserve(s.items.size());
                            for (const auto& item : s.items) {
                                if (item.is_star || !item.expr ||
                                    !std::holds_alternative<ColumnRefExpr>(item.expr->node)) {
                                    proj_ok = false;
                                    break;
                                }
                                const auto& cref = std::get<ColumnRefExpr>(item.expr->node);
                                if (!cref.table.empty() && !iequals_ascii(cref.table, base_qual)) {
                                    proj_ok = false;
                                    break;
                                }
                                plan.items.push_back(CompiledPkSelectPlan::ColItem{
                                    to_lower(cref.column),
                                    item.alias.empty() ? cref.column : item.alias
                                });
                            }
                        }

                        if (proj_ok) {
                            ps->pk_select_plan = std::move(plan);
                        }
                    }
                }
            }
        }
    }

    // Pre-compile single-column PK point-UPDATE plan
    if (ps->stmts.size() == 1 && std::holds_alternative<UpdateStmt>(ps->stmts[0])) {
        const auto& u = std::get<UpdateStmt>(ps->stmts[0]);
        if (u.assignments.size() == 1 && u.returning.empty() && u.where &&
            std::holds_alternative<BinaryExpr>(u.where->node)) {
            const auto& wbin = std::get<BinaryExpr>(u.where->node);
            if (wbin.op == BinaryOp::Eq && wbin.left && wbin.right) {
                const ColumnRefExpr* pk_cref = nullptr;
                const Expr* pk_rhs = nullptr;
                if (std::holds_alternative<ColumnRefExpr>(wbin.left->node)) {
                    pk_cref = &std::get<ColumnRefExpr>(wbin.left->node);
                    pk_rhs = wbin.right.get();
                } else if (std::holds_alternative<ColumnRefExpr>(wbin.right->node)) {
                    pk_cref = &std::get<ColumnRefExpr>(wbin.right->node);
                    pk_rhs = wbin.left.get();
                }

                bool pk_ok = false;
                uint32_t pk_param_idx = 0;
                int64_t pk_literal = 0;
                if (pk_cref && pk_rhs) {
                    if (std::holds_alternative<ParamExpr>(pk_rhs->node)) {
                        pk_param_idx = std::get<ParamExpr>(pk_rhs->node).index;
                        pk_ok = (pk_param_idx > 0);
                    } else if (std::holds_alternative<LiteralExpr>(pk_rhs->node)) {
                        const auto& lit = std::get<LiteralExpr>(pk_rhs->node);
                        if (std::holds_alternative<int64_t>(lit.value)) {
                            pk_literal = std::get<int64_t>(lit.value);
                            pk_ok = true;
                        }
                    }
                }

                if (pk_ok && u.assignments[0].value) {
                    CompiledPkUpdatePlan plan;
                    plan.table_lower = to_lower(u.table_name);
                    plan.target_col_lower = to_lower(u.assignments[0].column);
                    plan.pk_col_lower = to_lower(pk_cref->column);
                    plan.pk_param_idx = pk_param_idx;
                    plan.pk_literal = pk_literal;

                    const Expr* val_expr = u.assignments[0].value.get();
                    bool val_ok = false;
                    if (std::holds_alternative<ParamExpr>(val_expr->node)) {
                        plan.is_delta = false;
                        plan.val_param_idx = std::get<ParamExpr>(val_expr->node).index;
                        val_ok = (plan.val_param_idx > 0);
                    } else if (std::holds_alternative<LiteralExpr>(val_expr->node)) {
                        plan.is_delta = false;
                        plan.val_param_idx = 0;
                        plan.val_literal = eval_literal_expr(std::get<LiteralExpr>(val_expr->node));
                        val_ok = true;
                    } else if (std::holds_alternative<BinaryExpr>(val_expr->node)) {
                        const auto& vbin = std::get<BinaryExpr>(val_expr->node);
                        if ((vbin.op == BinaryOp::Add || vbin.op == BinaryOp::Sub) &&
                            vbin.left && vbin.right) {
                            const Expr* delta_expr = nullptr;
                            if (std::holds_alternative<ColumnRefExpr>(vbin.left->node) &&
                                iequals_ascii(std::get<ColumnRefExpr>(vbin.left->node).column,
                                              plan.target_col_lower)) {
                                delta_expr = vbin.right.get();
                                plan.is_sub = (vbin.op == BinaryOp::Sub);
                            } else if (vbin.op == BinaryOp::Add &&
                                       std::holds_alternative<ColumnRefExpr>(vbin.right->node) &&
                                       iequals_ascii(std::get<ColumnRefExpr>(vbin.right->node).column,
                                                     plan.target_col_lower)) {
                                delta_expr = vbin.left.get();
                                plan.is_sub = false;
                            }
                            if (delta_expr) {
                                plan.is_delta = true;
                                if (std::holds_alternative<ParamExpr>(delta_expr->node)) {
                                    plan.val_param_idx = std::get<ParamExpr>(delta_expr->node).index;
                                    val_ok = (plan.val_param_idx > 0);
                                } else if (std::holds_alternative<LiteralExpr>(delta_expr->node)) {
                                    plan.val_param_idx = 0;
                                    plan.val_literal = eval_literal_expr(
                                        std::get<LiteralExpr>(delta_expr->node));
                                    val_ok = (plan.val_literal.type == ColumnType::I64 ||
                                              plan.val_literal.type == ColumnType::F64);
                                }
                            }
                        }
                    }

                    if (val_ok) {
                        ps->pk_update_plan = std::move(plan);
                    }
                }
            }
        }
    }

    // Pre-compile single-row PK point-INSERT plan
    if (ps->stmts.size() == 1 && std::holds_alternative<InsertStmt>(ps->stmts[0])) {
        const auto& ins = std::get<InsertStmt>(ps->stmts[0]);
        if (ins.rows.size() == 1 && ins.returning.empty() && !ins.rows[0].empty()) {
            CompiledPkInsertPlan plan;
            plan.table_lower = to_lower(ins.table_name);
            plan.col_names_lower.reserve(ins.columns.size());
            for (const auto& c : ins.columns) {
                plan.col_names_lower.push_back(to_lower(c));
            }
            plan.values.reserve(ins.rows[0].size());
            bool all_simple = true;
            for (const auto& e : ins.rows[0]) {
                if (!e) { all_simple = false; break; }
                CompiledPkInsertPlan::Slot slot;
                if (std::holds_alternative<ParamExpr>(e->node)) {
                    slot.param_idx = std::get<ParamExpr>(e->node).index;
                    if (slot.param_idx == 0) { all_simple = false; break; }
                } else if (std::holds_alternative<LiteralExpr>(e->node)) {
                    slot.param_idx = 0;
                    slot.literal = eval_literal_expr(std::get<LiteralExpr>(e->node));
                } else {
                    all_simple = false;
                    break;
                }
                plan.values.push_back(std::move(slot));
            }
            if (all_simple) {
                ps->pk_insert_plan = std::move(plan);
            }
        }
    }

    return ps;
}

ZephyrResultSet* ZephyrEngine::try_exec_pk_select_plan(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    bool rcu_held
) {
    const auto& plan = *stmt.pk_select_plan;

    int64_t pk_val = plan.pk_literal;
    if (plan.pk_param_idx > 0) {
        if (!params || plan.pk_param_idx > param_count) return nullptr;
        const ZephyrValue& pv = params[plan.pk_param_idx - 1];
        if (pv.type != ZEPHYR_TYPE_I64) return nullptr;
        pk_val = pv.as.i64;
    }

    std::optional<cpp26::rcu::scoped_read_lock> local_rcu;
    if (!rcu_held) {
        local_rcu.emplace(reaper_.domain());
    }

    const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
    const Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
    if (!tbl) {
        return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + plan.table_lower);
    }

    if (!tbl->pk_is_i64 || tbl->pk_col < 0 ||
        !iequals_ascii(tbl->schema[tbl->pk_col].name, plan.pk_col_lower)) {
        return nullptr;
    }

    constexpr size_t STACK_COLS = 32;
    size_t col_count = plan.select_star ? tbl->schema.size() : plan.items.size();
    if (col_count > STACK_COLS) {
        return nullptr;
    }

    int proj_cols[STACK_COLS];
    if (plan.select_star) {
        for (size_t c = 0; c < col_count; ++c) {
            proj_cols[c] = static_cast<int>(c);
        }
    } else {
        for (size_t c = 0; c < col_count; ++c) {
            proj_cols[c] = tbl->find_col_nocase(plan.items[c].col_lower);
        }
    }

    const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val);
    std::lock_guard<std::mutex> shard_lk(tbl->row_shard_mu[shard]);
    if (in_txn_.load(std::memory_order_acquire)) {
        return nullptr;
    }

    auto pit = tbl->pk_shard_index[shard].find(pk_val);
    bool found = (pit != tbl->pk_shard_index[shard].end() && tbl->live[pit->second]);
    const size_t r_idx = found ? pit->second : 0;

    auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
    auto* rs = arena->create<ZephyrResultSet>();
    rs->_arena_handle = arena;
    rs->status = ZEPHYR_OK;
    rs->flags = 0;
    rs->rows_affected = 0;
    rs->col_count = col_count;
    rs->row_count = found ? 1 : 0;

    if (col_count > 0) {
        auto* cols = arena->alloc_array<ZephyrColumnDesc>(col_count);
        for (size_t c = 0; c < col_count; ++c) {
            if (plan.select_star) {
                cols[c].name = arena->dup_cstr(tbl->schema[c].name);
                cols[c].name_len = static_cast<uint32_t>(tbl->schema[c].name.size());
                cols[c].type = static_cast<int32_t>(tbl->schema[c].type);
            } else {
                const int src = proj_cols[c];
                cols[c].name = arena->dup_cstr(plan.items[c].label);
                cols[c].name_len = static_cast<uint32_t>(plan.items[c].label.size());
                cols[c].type = static_cast<int32_t>(src >= 0 ? tbl->schema[src].type : ColumnType::Null);
            }
        }
        rs->columns = cols;
    } else {
        rs->columns = nullptr;
    }

    if (found && col_count > 0) {
        auto* vals = arena->alloc_array<ZephyrValue>(col_count);
        for (size_t c = 0; c < col_count; ++c) {
            ZephyrValue& zv = vals[c];
            zv._pad = 0;
            const int src_col = proj_cols[c];
            if (src_col < 0) {
                zv.type = ZEPHYR_TYPE_NULL;
                zv.as.i64 = 0;
                continue;
            }
            const ColumnData& cdata = tbl->columns[src_col];
            if (cdata.nulls[r_idx]) {
                zv.type = ZEPHYR_TYPE_NULL;
                zv.as.i64 = 0;
                continue;
            }
            switch (cdata.type) {
                case ColumnType::I64:
                    zv.type = ZEPHYR_TYPE_I64;
                    zv.as.i64 = cdata.i64_data[r_idx];
                    break;
                case ColumnType::F64:
                    zv.type = ZEPHYR_TYPE_F64;
                    zv.as.f64 = cdata.f64_data[r_idx];
                    break;
                case ColumnType::Bool:
                    zv.type = ZEPHYR_TYPE_BOOL;
                    zv.as.boolean = cdata.bool_data[r_idx] ? 1 : 0;
                    break;
                case ColumnType::Text: {
                    const std::string& sv = cdata.str_data[r_idx];
                    zv.type = ZEPHYR_TYPE_TEXT;
                    zv.as.slice.len = static_cast<uint32_t>(sv.size());
                    zv.as.slice.reserved = 0;
                    zv.as.slice.ptr = sv.empty()
                        ? reinterpret_cast<const uint8_t*>("")
                        : arena->dup_bytes(sv.data(), sv.size());
                    break;
                }
                case ColumnType::Bytea: {
                    const std::string& sv = cdata.str_data[r_idx];
                    zv.type = ZEPHYR_TYPE_BYTEA;
                    zv.as.slice.len = static_cast<uint32_t>(sv.size());
                    zv.as.slice.reserved = 0;
                    zv.as.slice.ptr = sv.empty()
                        ? nullptr
                        : arena->dup_bytes(sv.data(), sv.size());
                    break;
                }
                case ColumnType::Null:
                    zv.type = ZEPHYR_TYPE_NULL;
                    zv.as.i64 = 0;
                    break;
            }
        }
        rs->values = vals;
    } else {
        rs->values = nullptr;
    }

    return rs;
}

bool ZephyrEngine::try_exec_pk_update_plan(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    int32_t& out_status,
    uint64_t& out_affected,
    bool rcu_held
) {
    const auto& plan = *stmt.pk_update_plan;
    out_status = ZEPHYR_OK;
    out_affected = 0;

    int64_t pk_val = plan.pk_literal;
    if (plan.pk_param_idx > 0) {
        if (!params || plan.pk_param_idx > param_count) return false;
        const ZephyrValue& pv = params[plan.pk_param_idx - 1];
        if (pv.type != ZEPHYR_TYPE_I64) return false;
        pk_val = pv.as.i64;
    }

    Value rhs_val;
    if (plan.val_param_idx > 0) {
        if (!params || plan.val_param_idx > param_count) return false;
        rhs_val = from_c_value(params[plan.val_param_idx - 1]);
    } else {
        rhs_val = plan.val_literal;
    }

    if (in_txn_.load(std::memory_order_acquire)) {
        return false;
    }

    // Lock-free RCU catalog lookup + single-shard mutex for POD columns
    // (ZERO global mu_ lock, ZERO table rw_mu lock!)
    {
        std::optional<cpp26::rcu::scoped_read_lock> local_rcu;
        if (!rcu_held) {
            local_rcu.emplace(reaper_.domain());
        }

        const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
        Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
        if (!tbl) {
            set_error("table not found: " + plan.table_lower);
            out_status = ZEPHYR_ERR_NOT_FOUND;
            return true;
        }

        if (!tbl->pk_is_i64 || tbl->pk_col < 0 ||
            !iequals_ascii(tbl->schema[tbl->pk_col].name, plan.pk_col_lower)) {
            return false;
        }

        int target_cidx = tbl->find_col_nocase(plan.target_col_lower);
        if (target_cidx < 0) {
            set_error("unknown column: " + plan.target_col_lower);
            out_status = ZEPHYR_ERR_NOT_FOUND;
            return true;
        }
        if (target_cidx == tbl->pk_col) {
            return false;
        }

        const ColumnSchema& cs = tbl->schema[target_cidx];
        const bool is_pod_col = (cs.type == ColumnType::I64 ||
                                 cs.type == ColumnType::F64 ||
                                 cs.type == ColumnType::Bool);

        if (is_pod_col) {
            const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val);
            std::lock_guard<std::mutex> shard_lk(tbl->row_shard_mu[shard]);
            if (in_txn_.load(std::memory_order_acquire)) {
                return false;
            }

            auto pit = tbl->pk_shard_index[shard].find(pk_val);
            if (pit == tbl->pk_shard_index[shard].end()) {
                out_status = ZEPHYR_OK;
                out_affected = 0;
                return true;
            }

            const size_t r = pit->second;
            ColumnData& cdata = tbl->columns[target_cidx];

            Value new_val;
            if (plan.is_delta) {
                if (cdata.nulls[r] || rhs_val.is_null()) {
                    new_val = Value::make_null();
                } else if (cs.type == ColumnType::I64 && rhs_val.type == ColumnType::I64) {
                    int64_t delta = plan.is_sub ? -rhs_val.i64 : rhs_val.i64;
                    new_val = Value::make_i64(cdata.i64_data[r] + delta);
                } else if (cs.type == ColumnType::F64 &&
                           (rhs_val.type == ColumnType::F64 || rhs_val.type == ColumnType::I64)) {
                    double d = (rhs_val.type == ColumnType::F64)
                        ? rhs_val.f64 : static_cast<double>(rhs_val.i64);
                    if (plan.is_sub) d = -d;
                    new_val = Value::make_f64(cdata.f64_data[r] + d);
                } else {
                    return false;
                }
            } else {
                std::string err;
                new_val = coerce_value(rhs_val, cs.type, err);
                if (!err.empty()) {
                    set_error(err);
                    out_status = ZEPHYR_ERR_INVALID_ARG;
                    return true;
                }
            }

            if (cs.not_null && new_val.is_null()) {
                set_error("NULL violates NOT NULL");
                out_status = ZEPHYR_ERR_CONSTRAINT;
                return true;
            }

            if (cs.type == ColumnType::I64) {
                int64_t old_v = cdata.nulls[r] ? 0 : cdata.i64_data[r];
                int64_t new_v = new_val.is_null() ? 0 : new_val.i64;
                int64_t delta = new_v - old_v;
                cdata.nulls[r] = new_val.is_null() ? 1 : 0;
                cdata.i64_data[r] = new_v;
                if (cs.agg_slot >= 0 && delta != 0) {
                    if (!tbl->btree->update_existing_i64(pk_val, static_cast<size_t>(cs.agg_slot), delta)) {
                        update_btree_for_row(*tbl, r);
                    }
                }
            } else if (cs.type == ColumnType::F64) {
                double old_v = cdata.nulls[r] ? 0.0 : cdata.f64_data[r];
                double new_v = new_val.is_null() ? 0.0 : new_val.f64;
                double delta = new_v - old_v;
                cdata.nulls[r] = new_val.is_null() ? 1 : 0;
                cdata.f64_data[r] = new_v;
                if (cs.agg_slot >= 0 && delta != 0.0) {
                    if (!tbl->btree->update_existing_f64(pk_val, static_cast<size_t>(cs.agg_slot), delta)) {
                        update_btree_for_row(*tbl, r);
                    }
                }
            } else {
                cdata.nulls[r] = new_val.is_null() ? 1 : 0;
                cdata.bool_data[r] = (!new_val.is_null() && new_val.boolean) ? 1 : 0;
            }

            if (!in_replay_ && wal_.is_open()) {
                if (param_count == 0) {
                    log_wal_entry(stmt.sql, false);
                } else {
                    std::string wal_sql = "UPDATE " + tbl->name + " SET " + cs.name + " = " +
                                          format_sql_literal(new_val) + " WHERE " +
                                          tbl->schema[tbl->pk_col].name + " = " +
                                          std::to_string(pk_val);
                    log_wal_entry(wal_sql, false);
                }
            }

            out_status = ZEPHYR_OK;
            out_affected = 1;
            return true;
        }
    }

    // Non-POD column (TEXT / BYTEA): acquire table locks + all shards
    std::shared_lock<std::shared_mutex> cat_lk(mu_);
    if (in_txn_.load(std::memory_order_acquire)) {
        return false;
    }

    const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
    Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
    if (!tbl) {
        set_error("table not found: " + plan.table_lower);
        out_status = ZEPHYR_ERR_NOT_FOUND;
        return true;
    }

    int target_cidx = tbl->find_col_nocase(plan.target_col_lower);
    if (target_cidx < 0) {
        set_error("unknown column: " + plan.target_col_lower);
        out_status = ZEPHYR_ERR_NOT_FOUND;
        return true;
    }
    const ColumnSchema& cs = tbl->schema[target_cidx];

    std::lock_guard<std::mutex> tbl_lk(tbl->write_mu);
    std::unique_lock<std::shared_mutex> row_lk(tbl->rw_mu);
    AllShardsLock shards_lk(*tbl);

    const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val);
    auto pit = tbl->pk_shard_index[shard].find(pk_val);
    if (pit == tbl->pk_shard_index[shard].end()) {
        out_status = ZEPHYR_OK;
        out_affected = 0;
        return true;
    }

    const size_t r = pit->second;
    ColumnData& cdata = tbl->columns[target_cidx];

    std::string err;
    Value new_val = coerce_value(rhs_val, cs.type, err);
    if (!err.empty()) {
        set_error(err);
        out_status = ZEPHYR_ERR_INVALID_ARG;
        return true;
    }

    if (cs.not_null && new_val.is_null()) {
        set_error("NULL violates NOT NULL");
        out_status = ZEPHYR_ERR_CONSTRAINT;
        return true;
    }

    cdata.set_value(r, new_val);
    update_btree_for_row(*tbl, r);

    if (!in_replay_ && wal_.is_open()) {
        if (param_count == 0) {
            log_wal_entry(stmt.sql, false);
        } else {
            std::string wal_sql = "UPDATE " + tbl->name + " SET " + cs.name + " = " +
                                  format_sql_literal(new_val) + " WHERE " +
                                  tbl->schema[tbl->pk_col].name + " = " +
                                  std::to_string(pk_val);
            log_wal_entry(wal_sql, false);
        }
    }

    out_status = ZEPHYR_OK;
    out_affected = 1;
    return true;
}

bool ZephyrEngine::try_exec_pk_insert_plan(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    int32_t& out_status,
    uint64_t& out_affected
) {
    const auto& plan = *stmt.pk_insert_plan;
    out_status = ZEPHYR_OK;
    out_affected = 0;

    std::shared_lock<std::shared_mutex> cat_lk(mu_);
    if (in_txn_.load(std::memory_order_acquire)) {
        return false;
    }

    const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
    Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
    if (!tbl) {
        set_error("table not found: " + plan.table_lower);
        out_status = ZEPHYR_ERR_NOT_FOUND;
        return true;
    }

    const size_t ncols = tbl->schema.size();
    std::vector<size_t> target_cols;
    if (plan.col_names_lower.empty()) {
        if (plan.values.size() != ncols) {
            set_error("INSERT column count mismatch");
            out_status = ZEPHYR_ERR_INVALID_ARG;
            return true;
        }
        target_cols.resize(ncols);
        for (size_t i = 0; i < ncols; ++i) target_cols[i] = i;
    } else {
        if (plan.values.size() != plan.col_names_lower.size()) {
            set_error("INSERT column count mismatch");
            out_status = ZEPHYR_ERR_INVALID_ARG;
            return true;
        }
        target_cols.reserve(plan.col_names_lower.size());
        for (const auto& cname : plan.col_names_lower) {
            int idx = tbl->find_col_nocase(cname);
            if (idx < 0) {
                set_error("unknown column: " + cname);
                out_status = ZEPHYR_ERR_NOT_FOUND;
                return true;
            }
            target_cols.push_back(static_cast<size_t>(idx));
        }
    }

    std::vector<Value> full_row(ncols, Value::make_null());
    for (size_t j = 0; j < target_cols.size(); ++j) {
        const auto& slot = plan.values[j];
        Value raw;
        if (slot.param_idx > 0) {
            if (params && slot.param_idx <= param_count) {
                raw = from_c_value(params[slot.param_idx - 1]);
            } else {
                raw = Value::make_null();
            }
        } else {
            raw = slot.literal;
        }
        std::string err;
        Value coerced = coerce_value(raw, tbl->schema[target_cols[j]].type, err);
        if (!err.empty()) {
            set_error(err);
            out_status = ZEPHYR_ERR_INVALID_ARG;
            return true;
        }
        full_row[target_cols[j]] = std::move(coerced);
    }

    for (size_t c = 0; c < ncols; ++c) {
        if (tbl->schema[c].not_null && full_row[c].is_null()) {
            set_error("NULL value violates NOT NULL on " + tbl->schema[c].name);
            out_status = ZEPHYR_ERR_CONSTRAINT;
            return true;
        }
    }

    std::lock_guard<std::mutex> tbl_lk(tbl->write_mu);
    std::unique_lock<std::shared_mutex> row_lk(tbl->rw_mu);
    AllShardsLock shards_lk(*tbl);

    if (tbl->pk_col >= 0) {
        const Value& pk_val = full_row[tbl->pk_col];
        if (tbl->pk_is_i64) {
            const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val.i64);
            if (tbl->pk_shard_index[shard].count(pk_val.i64)) {
                set_error("duplicate primary key");
                out_status = ZEPHYR_ERR_CONSTRAINT;
                return true;
            }
        } else {
            if (tbl->pk_str_index.count(value_key_string(pk_val))) {
                set_error("duplicate primary key");
                out_status = ZEPHYR_ERR_CONSTRAINT;
                return true;
            }
        }
    }

    const size_t row_idx = tbl->live.size();
    for (size_t c = 0; c < ncols; ++c) {
        tbl->columns[c].push_value(full_row[c]);
    }
    tbl->live.push_back(1);

    if (tbl->pk_col >= 0) {
        const Value& pk_val = full_row[tbl->pk_col];
        if (tbl->pk_is_i64) {
            const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val.i64);
            tbl->pk_shard_index[shard][pk_val.i64] = row_idx;
        } else {
            tbl->pk_str_index[value_key_string(pk_val)] = row_idx;
        }
    }
    update_btree_for_row(*tbl, row_idx);

    if (!in_replay_ && wal_.is_open()) {
        if (param_count == 0) {
            log_wal_entry(stmt.sql, false);
        } else {
            std::string wal_sql = "INSERT INTO " + tbl->name + " VALUES (";
            for (size_t c = 0; c < ncols; ++c) {
                if (c > 0) wal_sql += ", ";
                wal_sql += format_sql_literal(full_row[c]);
            }
            wal_sql += ")";
            log_wal_entry(wal_sql, false);
        }
    }

    out_status = ZEPHYR_OK;
    out_affected = 1;
    return true;
}

ZephyrResultSet* ZephyrEngine::query_prepared(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count
) {
    return query_prepared_impl(stmt, params, param_count, /*rcu_held=*/false);
}

ZephyrResultSet* ZephyrEngine::query_prepared_impl(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    bool rcu_held
) {
    clear_error();
    stats_slots_[stat_slot()].prep_queries.fetch_add(1, std::memory_order_relaxed);

    // Lock-free RCU O(1) fastpath: zero shared_mutex, zero std::vector/std::string allocations
    if (stmt.o1_plan.has_value() && !in_txn_.load(std::memory_order_acquire)) {
        const auto& plan = *stmt.o1_plan;
        const size_t n = plan.items.size();
        constexpr size_t STACK_CAP = 16;

        if (n <= STACK_CAP) {
            // Pre-copy labels into arena when n == 1 so we never dereference `plan` after inner RCU exits
            if (n == 1) {
                std::optional<cpp26::rcu::scoped_read_lock> local_rcu;
                if (!rcu_held) {
                    local_rcu.emplace(reaper_.domain());
                }
                const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
                const Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
                if (tbl) {
                    const auto& item = plan.items[0];
                    auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
                    auto* rs = arena->create<ZephyrResultSet>();
                    auto* cols = arena->alloc_array<ZephyrColumnDesc>(1);
                    auto* vals = arena->alloc_array<ZephyrValue>(1);
                    cols[0].name = arena->dup_cstr(item.label);
                    cols[0].name_len = static_cast<uint32_t>(item.label.size());
                    vals[0]._pad = 0;

                    bool ok = true;
                    if (item.is_count) {
                        cols[0].type = static_cast<int32_t>(ColumnType::I64);
                        vals[0].type = ZEPHYR_TYPE_I64;
                        vals[0].as.i64 = static_cast<int64_t>(tbl->btree->total_count());
                    } else {
                        auto cit = tbl->col_map.find(item.col_lower);
                        if (cit == tbl->col_map.end() || tbl->schema[cit->second].agg_slot < 0) {
                            ok = false;
                        } else {
                            const ColumnSchema& cs = tbl->schema[cit->second];
                            const size_t slot = static_cast<size_t>(cs.agg_slot);
                            if (cs.type == ColumnType::I64) {
                                cols[0].type = static_cast<int32_t>(ColumnType::I64);
                                vals[0].type = ZEPHYR_TYPE_I64;
                                vals[0].as.i64 = tbl->btree->total_sum_i64(slot);
                            } else if (cs.type == ColumnType::F64) {
                                cols[0].type = static_cast<int32_t>(ColumnType::F64);
                                vals[0].type = ZEPHYR_TYPE_F64;
                                vals[0].as.f64 = tbl->btree->total_sum_f64(slot);
                            } else {
                                ok = false;
                            }
                        }
                    }

                    if (ok) {
                        stats_slots_[stat_slot()].o1_hits.fetch_add(1, std::memory_order_relaxed);
                        rs->_arena_handle = arena;
                        rs->status = ZEPHYR_OK;
                        rs->flags = ZEPHYR_FLAG_O1_FASTPATH;
                        rs->rows_affected = 0;
                        rs->col_count = 1;
                        rs->row_count = 1;
                        rs->columns = cols;
                        rs->values = vals;
                        return rs;
                    }
                    release_arena(arena);
                }
            } else {
                ColumnType out_types[STACK_CAP];
                int64_t out_i64[STACK_CAP];
                double out_f64[STACK_CAP];
                bool ok = true;

                for (size_t i = 0; i < n; ++i) {
                    cpp26::rcu::scoped_read_lock rcu_guard(reaper_.domain());
                    const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
                    const Table* tbl = snap ? snap->find(plan.table_lower) : nullptr;
                    if (!tbl) {
                        ok = false;
                        break;
                    }

                    const auto& item = plan.items[i];
                    if (item.is_count) {
                        out_types[i] = ColumnType::I64;
                        out_i64[i] = static_cast<int64_t>(tbl->btree->total_count());
                    } else {
                        auto cit = tbl->col_map.find(item.col_lower);
                        if (cit == tbl->col_map.end()) {
                            ok = false;
                            break;
                        }
                        const ColumnSchema& cs = tbl->schema[cit->second];
                        if (cs.agg_slot < 0) {
                            ok = false;
                            break;
                        }
                        const size_t slot = static_cast<size_t>(cs.agg_slot);
                        if (cs.type == ColumnType::I64) {
                            out_types[i] = ColumnType::I64;
                            out_i64[i] = tbl->btree->total_sum_i64(slot);
                        } else if (cs.type == ColumnType::F64) {
                            out_types[i] = ColumnType::F64;
                            out_f64[i] = tbl->btree->total_sum_f64(slot);
                        } else {
                            ok = false;
                            break;
                        }
                    }
                }

                if (ok) {
                    stats_slots_[stat_slot()].o1_hits.fetch_add(1, std::memory_order_relaxed);
                    auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
                    auto* rs = arena->create<ZephyrResultSet>();
                    rs->_arena_handle = arena;
                    rs->status = ZEPHYR_OK;
                    rs->flags = ZEPHYR_FLAG_O1_FASTPATH;
                    rs->rows_affected = 0;
                    rs->col_count = n;
                    rs->row_count = 1;

                    auto* cols = arena->alloc_array<ZephyrColumnDesc>(n);
                    auto* vals = arena->alloc_array<ZephyrValue>(n);
                    for (size_t i = 0; i < n; ++i) {
                        cols[i].name = arena->dup_cstr(plan.items[i].label);
                        cols[i].name_len = static_cast<uint32_t>(plan.items[i].label.size());
                        cols[i].type = static_cast<int32_t>(out_types[i]);

                        vals[i]._pad = 0;
                        if (out_types[i] == ColumnType::I64) {
                            vals[i].type = ZEPHYR_TYPE_I64;
                            vals[i].as.i64 = out_i64[i];
                        } else {
                            vals[i].type = ZEPHYR_TYPE_F64;
                            vals[i].as.f64 = out_f64[i];
                        }
                    }
                    rs->columns = cols;
                    rs->values = vals;
                    return rs;
                }
            }
        }
    }

    // O(1) PK point-lookup SELECT fastpath (zero SIMD scan, zero std::vector heap allocs)
    if (stmt.pk_select_plan.has_value() && !in_txn_.load(std::memory_order_acquire)) {
        if (ZephyrResultSet* rs = try_exec_pk_select_plan(stmt, params, param_count, rcu_held)) {
            return rs;
        }
    }

    // Precompiled PK point-UPDATE fastpath
    if (stmt.pk_update_plan.has_value() && !in_txn_.load(std::memory_order_acquire)) {
        int32_t st = ZEPHYR_OK;
        uint64_t affected = 0;
        if (try_exec_pk_update_plan(stmt, params, param_count, st, affected, rcu_held)) {
            if (st != ZEPHYR_OK) {
                return make_error_result(static_cast<uint32_t>(st), last_error());
            }
            return pack_result({}, {}, {}, affected, 0);
        }
    }

    // Precompiled PK point-INSERT fastpath
    if (stmt.pk_insert_plan.has_value() && !in_txn_.load(std::memory_order_acquire)) {
        int32_t st = ZEPHYR_OK;
        uint64_t affected = 0;
        if (try_exec_pk_insert_plan(stmt, params, param_count, st, affected)) {
            if (st != ZEPHYR_OK) {
                return make_error_result(static_cast<uint32_t>(st), last_error());
            }
            return pack_result({}, {}, {}, affected, 0);
        }
    }

    return execute_stmts(stmt.sql, stmt.stmts, params, param_count, false);
}

int32_t ZephyrEngine::exec_prepared(
    const PreparedStmt& stmt,
    const ZephyrValue* params,
    uint32_t param_count,
    uint64_t* rows_affected
) {
    // Direct zero-arena fastpath for precompiled PK point-UPDATE / point-INSERT
    if (!in_txn_.load(std::memory_order_acquire)) {
        if (stmt.pk_update_plan.has_value()) {
            clear_error();
            stats_slots_[stat_slot()].prep_queries.fetch_add(1, std::memory_order_relaxed);
            int32_t st = ZEPHYR_OK;
            uint64_t affected = 0;
            if (try_exec_pk_update_plan(stmt, params, param_count, st, affected)) {
                if (rows_affected) *rows_affected = affected;
                return st;
            }
            // Fall through to execute_stmts (already counted in prepared_queries_)
            ZephyrResultSet* res = execute_stmts(stmt.sql, stmt.stmts, params, param_count, false);
            if (!res) return ZEPHYR_ERR_INTERNAL;
            int32_t status = static_cast<int32_t>(res->status);
            if (rows_affected) *rows_affected = res->rows_affected;
            free_result(res);
            return status;
        }
        if (stmt.pk_insert_plan.has_value()) {
            clear_error();
            stats_slots_[stat_slot()].prep_queries.fetch_add(1, std::memory_order_relaxed);
            int32_t st = ZEPHYR_OK;
            uint64_t affected = 0;
            if (try_exec_pk_insert_plan(stmt, params, param_count, st, affected)) {
                if (rows_affected) *rows_affected = affected;
                return st;
            }
            ZephyrResultSet* res = execute_stmts(stmt.sql, stmt.stmts, params, param_count, false);
            if (!res) return ZEPHYR_ERR_INTERNAL;
            int32_t status = static_cast<int32_t>(res->status);
            if (rows_affected) *rows_affected = res->rows_affected;
            free_result(res);
            return status;
        }
    }

    ZephyrResultSet* res = query_prepared(stmt, params, param_count);
    if (!res) return ZEPHYR_ERR_INTERNAL;
    int32_t status = static_cast<int32_t>(res->status);
    if (rows_affected) {
        *rows_affected = res->rows_affected;
    }
    free_result(res);
    return status;
}

ZephyrResultSet* ZephyrEngine::execute_stmts(
    std::string_view raw_sql,
    const std::vector<Stmt>& stmts,
    const ZephyrValue* params,
    uint32_t param_count,
    bool sync_wal
) {
    std::vector<Value> cpp_params;
    if (params && param_count > 0) {
        cpp_params.reserve(param_count);
        for (uint32_t i = 0; i < param_count; ++i) {
            cpp_params.push_back(from_c_value(params[i]));
        }
    }

    if (stmts.empty()) {
        return pack_result({}, {}, {}, 0, 0);
    }

    if (stmts.size() == 1 && !in_txn_.load(std::memory_order_acquire)) {
        if (std::holds_alternative<SelectStmt>(stmts[0])) {
            std::shared_lock<std::shared_mutex> lk(mu_);
            if (!in_txn_.load(std::memory_order_acquire)) {
                ZephyrResultSet* res = exec_stmt(stmts[0], raw_sql, cpp_params, false, false);
                if (!res) {
                    return make_error_result(ZEPHYR_ERR_INTERNAL, "null execution result");
                }
                return res;
            }
        } else if (std::holds_alternative<InsertStmt>(stmts[0]) ||
                   std::holds_alternative<UpdateStmt>(stmts[0]) ||
                   std::holds_alternative<DeleteStmt>(stmts[0])) {
            std::shared_lock<std::shared_mutex> cat_lk(mu_);
            if (!in_txn_.load(std::memory_order_acquire)) {
                std::string_view tname;
                if (const auto* ins = std::get_if<InsertStmt>(&stmts[0])) tname = ins->table_name;
                else if (const auto* upd = std::get_if<UpdateStmt>(&stmts[0])) tname = upd->table_name;
                else if (const auto* del = std::get_if<DeleteStmt>(&stmts[0])) tname = del->table_name;

                char lower_buf[128];
                std::string lower_heap;
                std::string_view key = to_lower_sv(tname, lower_buf, sizeof(lower_buf), lower_heap);
                const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
                Table* tbl = snap ? snap->find(key) : nullptr;
                if (!tbl) {
                    return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + std::string(tname));
                }

                std::lock_guard<std::mutex> tbl_lk(tbl->write_mu);
                std::unique_lock<std::shared_mutex> row_lk(tbl->rw_mu);
                AllShardsLock shards_lk(*tbl);
                ZephyrResultSet* res = exec_stmt(stmts[0], raw_sql, cpp_params, true, sync_wal);
                if (!res) {
                    return make_error_result(ZEPHYR_ERR_INTERNAL, "null execution result");
                }
                return res;
            }
        }
    }

    std::unique_lock<std::shared_mutex> lk(mu_);
    ZephyrResultSet* last_res = nullptr;
    uint64_t total_affected = 0;
    for (size_t i = 0; i < stmts.size(); ++i) {
        if (last_res) {
            free_result(last_res);
            last_res = nullptr;
        }
        std::optional<AllShardsLock> shards_lk;
        std::string_view tname;
        if (const auto* ins = std::get_if<InsertStmt>(&stmts[i])) tname = ins->table_name;
        else if (const auto* upd = std::get_if<UpdateStmt>(&stmts[i])) tname = upd->table_name;
        else if (const auto* del = std::get_if<DeleteStmt>(&stmts[i])) tname = del->table_name;
        if (!tname.empty()) {
            auto it = tables_.find(to_lower(tname));
            if (it != tables_.end() && it->second) {
                shards_lk.emplace(*it->second);
            }
        }
        last_res = exec_stmt(stmts[i], raw_sql, cpp_params, stmts.size() == 1, sync_wal);
        if (!last_res) {
            return make_error_result(ZEPHYR_ERR_INTERNAL, "null execution result");
        }
        total_affected += last_res->rows_affected;
        if (last_res->status != ZEPHYR_OK) {
            return last_res;
        }
    }
    if (stmts.size() > 1 && last_res) {
        last_res->rows_affected = total_affected;
        if (sync_wal && !in_replay_ && !in_txn_.load(std::memory_order_acquire) && wal_.is_open()) {
            log_wal_entry(raw_sql, true);
        }
    }
    return last_res;
}

ZephyrAggregate ZephyrEngine::aggregate_count(std::string_view table_name) {
    ZephyrAggregate agg{};
    char lower_buf[128];
    std::string lower_heap;
    std::string_view key = to_lower_sv(table_name, lower_buf, sizeof(lower_buf), lower_heap);

    cpp26::rcu::scoped_read_lock rcu_guard(reaper_.domain());
    const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
    const Table* tbl = snap ? snap->find(key) : nullptr;
    if (!tbl) {
        agg.status = ZEPHYR_ERR_NOT_FOUND;
        set_error("table not found: " + std::string(table_name));
        return agg;
    }
    agg.count = static_cast<int64_t>(tbl->btree->total_count());
    agg.status = ZEPHYR_OK;
    stats_slots_[stat_slot()].o1_hits.fetch_add(1, std::memory_order_relaxed);
    return agg;
}

ZephyrAggregate ZephyrEngine::aggregate_sum(std::string_view table_name, std::string_view col_name) {
    ZephyrAggregate agg{};
    char lower_buf[128];
    std::string lower_heap;
    std::string_view key = to_lower_sv(table_name, lower_buf, sizeof(lower_buf), lower_heap);

    bool need_fallback_scan = false;
    {
        cpp26::rcu::scoped_read_lock rcu_guard(reaper_.domain());
        const CatalogSnapshot* snap = catalog_snap_.load(std::memory_order_acquire);
        const Table* tbl = snap ? snap->find(key) : nullptr;
        if (!tbl) {
            agg.status = ZEPHYR_ERR_NOT_FOUND;
            set_error("table not found: " + std::string(table_name));
            return agg;
        }
        int col_idx = tbl->find_col_nocase(col_name);
        if (col_idx < 0) {
            agg.status = ZEPHYR_ERR_NOT_FOUND;
            set_error("column not found: " + std::string(col_name));
            return agg;
        }
        const ColumnSchema& cs = tbl->schema[col_idx];
        if (cs.agg_slot >= 0) {
            const size_t slot = static_cast<size_t>(cs.agg_slot);
            if (cs.type == ColumnType::I64) {
                agg.sum_i64 = tbl->btree->total_sum_i64(slot);
                agg.sum_f64 = static_cast<double>(agg.sum_i64);
            } else if (cs.type == ColumnType::F64) {
                agg.sum_f64 = tbl->btree->total_sum_f64(slot);
            } else {
                agg.status = ZEPHYR_ERR_INVALID_ARG;
                set_error("SUM requires numeric column");
                return agg;
            }
            // Re-establish RCU read lock for total_count() after inner guard exit
            cpp26::rcu::scoped_read_lock rcu_guard2(reaper_.domain());
            const CatalogSnapshot* snap2 = catalog_snap_.load(std::memory_order_acquire);
            const Table* tbl2 = snap2 ? snap2->find(key) : nullptr;
            if (tbl2) {
                agg.count = static_cast<int64_t>(tbl2->btree->total_count());
            }
            agg.status = ZEPHYR_OK;
            stats_slots_[stat_slot()].o1_hits.fetch_add(1, std::memory_order_relaxed);
            return agg;
        } else if (cs.type == ColumnType::I64 || cs.type == ColumnType::F64) {
            need_fallback_scan = true;
        } else {
            agg.status = ZEPHYR_ERR_INVALID_ARG;
            set_error("SUM requires numeric column");
            return agg;
        }
    }

    if (need_fallback_scan) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = tables_.find(std::string(key));
        if (it == tables_.end()) {
            agg.status = ZEPHYR_ERR_NOT_FOUND;
            set_error("table not found: " + std::string(table_name));
            return agg;
        }
        const Table& tbl = *it->second;
        std::shared_lock<std::shared_mutex> row_lk(tbl.rw_mu);
        int col_idx = tbl.find_col_nocase(col_name);
        if (col_idx < 0) {
            agg.status = ZEPHYR_ERR_NOT_FOUND;
            set_error("column not found: " + std::string(col_name));
            return agg;
        }
        const ColumnSchema& cs = tbl.schema[col_idx];
        agg.count = static_cast<int64_t>(tbl.btree->total_count());
        for (size_t r = 0; r < tbl.live.size(); ++r) {
            if (!tbl.live[r] || tbl.columns[col_idx].nulls[r]) continue;
            if (cs.type == ColumnType::I64) {
                agg.sum_i64 += tbl.columns[col_idx].i64_data[r];
            } else {
                agg.sum_f64 += tbl.columns[col_idx].f64_data[r];
            }
        }
        if (cs.type == ColumnType::I64) agg.sum_f64 = static_cast<double>(agg.sum_i64);
    }
    agg.status = ZEPHYR_OK;
    return agg;
}

ZephyrStats ZephyrEngine::get_stats() const {
    ZephyrStats stats{};
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        stats.table_count = tables_.size();
        uint64_t rows = 0;
        for (const auto& [k, tbl] : tables_) {
            if (tbl && tbl->btree) {
                rows += tbl->btree->total_count();
            }
        }
        stats.total_rows = rows;
    }
    if (wal_.is_open()) {
        stats.wal_lsn = wal_.last_lsn();
        stats.wal_flushed_lsn = wal_.flushed_lsn();
    }
    stats.rcu_reclaimed = reaper_.reclaimed_total();
    uint64_t o1_total = 0;
    uint64_t prep_total = 0;
    for (const auto& slot : stats_slots_) {
        o1_total += slot.o1_hits.load(std::memory_order_relaxed);
        prep_total += slot.prep_queries.load(std::memory_order_relaxed);
    }
    stats.o1_fastpath_hits = o1_total;
    stats.prepared_queries = prep_total;
    stats.active_txns = active_txns_.load(std::memory_order_relaxed);
    return stats;
}

int32_t ZephyrEngine::checkpoint() {
    std::unique_lock<std::shared_mutex> lk(mu_);
    if (snap_path_.empty()) {
        return ZEPHYR_OK;
    }
    auto blob = serialize_snapshot();
    mmap_store_.close_mmap();
    bool wrote = encrypted_
        ? mmap_store_.write_snapshot_encrypted(snap_path_, blob, snap_key_)
        : mmap_store_.write_snapshot(snap_path_, blob);
    if (!wrote) {
        set_error("failed to write snapshot");
        return ZEPHYR_ERR_IO;
    }
    if (wal_.is_open()) {
        wal_.truncate();
    }
    return ZEPHYR_OK;
}

const char* ZephyrEngine::result_to_json(const ZephyrResultSet* result, uint32_t* out_len) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_json_ = reflection::result_set_to_json(result);
    if (out_len) {
        *out_len = static_cast<uint32_t>(last_json_.size());
    }
    return last_json_.c_str();
}

// ============================================================================
// Result Packing & Error Helpers
// ============================================================================

void ZephyrEngine::set_error(std::string_view msg) {
    std::lock_guard<std::mutex> lk(err_mu_);
    last_error_.assign(msg);
    has_error_.store(true, std::memory_order_release);
}

void ZephyrEngine::clear_error() {
    if (has_error_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(err_mu_);
        last_error_.clear();
        has_error_.store(false, std::memory_order_release);
    }
}

ZephyrResultSet* ZephyrEngine::make_error_result(uint32_t status, const std::string& msg) {
    set_error(msg);
    auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
    auto* rs = arena->create<ZephyrResultSet>();
    rs->row_count = 0;
    rs->col_count = 0;
    rs->rows_affected = 0;
    rs->columns = nullptr;
    rs->values = nullptr;
    rs->_arena_handle = arena;
    rs->status = status;
    rs->flags = 0;
    return rs;
}

ZephyrResultSet* ZephyrEngine::pack_result(
    const std::vector<std::string>& col_names,
    const std::vector<ColumnType>& col_types,
    const std::vector<std::vector<Value>>& rows,
    uint64_t rows_affected,
    uint32_t flags
) {
    auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
    auto* rs = arena->create<ZephyrResultSet>();
    rs->_arena_handle = arena;
    rs->status = ZEPHYR_OK;
    rs->flags = flags;
    rs->rows_affected = rows_affected;
    rs->col_count = col_names.size();
    rs->row_count = rows.size();

    if (rs->col_count > 0) {
        auto* cols = arena->alloc_array<ZephyrColumnDesc>(rs->col_count);
        for (size_t c = 0; c < col_names.size(); ++c) {
            cols[c].name = arena->dup_cstr(col_names[c]);
            cols[c].name_len = static_cast<uint32_t>(col_names[c].size());
            cols[c].type = static_cast<int32_t>(col_types[c]);
        }
        rs->columns = cols;
    } else {
        rs->columns = nullptr;
    }

    if (rs->row_count > 0 && rs->col_count > 0) {
        auto* vals = arena->alloc_array<ZephyrValue>(rs->row_count * rs->col_count);
        for (size_t r = 0; r < rows.size(); ++r) {
            for (size_t c = 0; c < col_names.size(); ++c) {
                vals[r * rs->col_count + c] = to_c_value(rows[r][c], *arena);
            }
        }
        rs->values = vals;
    } else {
        rs->values = nullptr;
    }

    return rs;
}

// ============================================================================
// Expression Evaluation
// ============================================================================

bool ZephyrEngine::truthy(const Value& v) const noexcept {
    switch (v.type) {
        case ColumnType::Null: return false;
        case ColumnType::Bool: return v.boolean;
        case ColumnType::I64: return v.i64 != 0;
        case ColumnType::F64: return v.f64 != 0.0;
        case ColumnType::Text:
        case ColumnType::Bytea: return !v.str.empty();
    }
    return false;
}

int ZephyrEngine::compare_values(const Value& a, const Value& b) const noexcept {
    if (a.is_null() && b.is_null()) return 0;
    if (a.is_null()) return -1;
    if (b.is_null()) return 1;

    if ((a.type == ColumnType::I64 || a.type == ColumnType::F64) &&
        (b.type == ColumnType::I64 || b.type == ColumnType::F64)) {
        if (a.type == ColumnType::I64 && b.type == ColumnType::I64) {
            return (a.i64 < b.i64) ? -1 : (a.i64 > b.i64 ? 1 : 0);
        }
        double da = (a.type == ColumnType::I64) ? static_cast<double>(a.i64) : a.f64;
        double db = (b.type == ColumnType::I64) ? static_cast<double>(b.i64) : b.f64;
        return (da < db) ? -1 : (da > db ? 1 : 0);
    }

    if (a.type == ColumnType::Bool && b.type == ColumnType::Bool) {
        return (a.boolean == b.boolean) ? 0 : (a.boolean ? 1 : -1);
    }

    return a.str.compare(b.str);
}

Value ZephyrEngine::coerce_value(const Value& v, ColumnType target, std::string& err) {
    if (v.is_null()) return v;
    if (v.type == target) return v;

    if (target == ColumnType::F64 && v.type == ColumnType::I64) {
        return Value::make_f64(static_cast<double>(v.i64));
    }
    if (target == ColumnType::I64 && v.type == ColumnType::F64) {
        return Value::make_i64(static_cast<int64_t>(v.f64));
    }
    if (target == ColumnType::Bytea && v.type == ColumnType::Text) {
        return Value::make_bytea(v.str);
    }
    if (target == ColumnType::Text && v.type == ColumnType::Bytea) {
        return Value::make_text(v.str);
    }
    err = "type mismatch";
    return Value::make_null();
}

Value ZephyrEngine::eval_expr(const Expr& expr, const EvalContext& ctx) {
    return std::visit([&](const auto& node) -> Value {
        using T = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<T, LiteralExpr>) {
            return std::visit([](const auto& val) -> Value {
                using V = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<V, std::nullptr_t>) return Value::make_null();
                else if constexpr (std::is_same_v<V, int64_t>) return Value::make_i64(val);
                else if constexpr (std::is_same_v<V, double>) return Value::make_f64(val);
                else if constexpr (std::is_same_v<V, bool>) return Value::make_bool(val);
                else return Value::make_text(val);
            }, node.value);
        } else if constexpr (std::is_same_v<T, ParamExpr>) {
            if (!ctx.params || node.index == 0 || node.index > ctx.params->size()) {
                return Value::make_null();
            }
            return (*ctx.params)[node.index - 1];
        } else if constexpr (std::is_same_v<T, ColumnRefExpr>) {
            if (!ctx.col_names || !ctx.row) return Value::make_null();
            std::string target_col = to_lower(node.column);
            std::string target_tbl = to_lower(node.table);
            for (size_t i = 0; i < ctx.col_names->size(); ++i) {
                if (to_lower((*ctx.col_names)[i]) == target_col) {
                    if (target_tbl.empty() ||
                        (ctx.col_qualifiers && to_lower((*ctx.col_qualifiers)[i]) == target_tbl)) {
                        return (*ctx.row)[i];
                    }
                }
            }
            return Value::make_null();
        } else if constexpr (std::is_same_v<T, UnaryExpr>) {
            Value v = eval_expr(*node.operand, ctx);
            if (node.op == UnaryOp::Not) {
                if (v.is_null()) return Value::make_null();
                return Value::make_bool(!truthy(v));
            }
            if (v.is_null()) return Value::make_null();
            if (v.type == ColumnType::I64) return Value::make_i64(-v.i64);
            if (v.type == ColumnType::F64) return Value::make_f64(-v.f64);
            return Value::make_null();
        } else if constexpr (std::is_same_v<T, IsNullExpr>) {
            Value v = eval_expr(*node.operand, ctx);
            bool is_n = v.is_null();
            return Value::make_bool(node.is_not ? !is_n : is_n);
        } else if constexpr (std::is_same_v<T, BetweenExpr>) {
            Value v = eval_expr(*node.expr, ctx);
            Value lo = eval_expr(*node.low, ctx);
            Value hi = eval_expr(*node.high, ctx);
            if (v.is_null() || lo.is_null() || hi.is_null()) return Value::make_bool(false);
            bool in_range = compare_values(v, lo) >= 0 && compare_values(v, hi) <= 0;
            return Value::make_bool(node.is_not ? !in_range : in_range);
        } else if constexpr (std::is_same_v<T, BinaryExpr>) {
            if (node.op == BinaryOp::And) {
                Value l = eval_expr(*node.left, ctx);
                if (!truthy(l)) return Value::make_bool(false);
                Value r = eval_expr(*node.right, ctx);
                return Value::make_bool(truthy(r));
            }
            if (node.op == BinaryOp::Or) {
                Value l = eval_expr(*node.left, ctx);
                if (truthy(l)) return Value::make_bool(true);
                Value r = eval_expr(*node.right, ctx);
                return Value::make_bool(truthy(r));
            }
            Value l = eval_expr(*node.left, ctx);
            Value r = eval_expr(*node.right, ctx);
            if (l.is_null() || r.is_null()) {
                return Value::make_null();
            }
            switch (node.op) {
                case BinaryOp::Eq: return Value::make_bool(compare_values(l, r) == 0);
                case BinaryOp::Ne: return Value::make_bool(compare_values(l, r) != 0);
                case BinaryOp::Lt: return Value::make_bool(compare_values(l, r) < 0);
                case BinaryOp::Le: return Value::make_bool(compare_values(l, r) <= 0);
                case BinaryOp::Gt: return Value::make_bool(compare_values(l, r) > 0);
                case BinaryOp::Ge: return Value::make_bool(compare_values(l, r) >= 0);
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div: {
                    if (l.type == ColumnType::I64 && r.type == ColumnType::I64) {
                        if (node.op == BinaryOp::Add) return Value::make_i64(l.i64 + r.i64);
                        if (node.op == BinaryOp::Sub) return Value::make_i64(l.i64 - r.i64);
                        if (node.op == BinaryOp::Mul) return Value::make_i64(l.i64 * r.i64);
                        if (r.i64 == 0) return Value::make_null();
                        return Value::make_i64(l.i64 / r.i64);
                    }
                    double dl = (l.type == ColumnType::I64) ? static_cast<double>(l.i64) : l.f64;
                    double dr = (r.type == ColumnType::I64) ? static_cast<double>(r.i64) : r.f64;
                    if (node.op == BinaryOp::Add) return Value::make_f64(dl + dr);
                    if (node.op == BinaryOp::Sub) return Value::make_f64(dl - dr);
                    if (node.op == BinaryOp::Mul) return Value::make_f64(dl * dr);
                    if (dr == 0.0) return Value::make_null();
                    return Value::make_f64(dl / dr);
                }
                default:
                    return Value::make_null();
            }
        } else {
            return Value::make_null();
        }
    }, expr.node);
}

// ============================================================================
// Statement Dispatch & WAL
// ============================================================================

void ZephyrEngine::log_wal_entry(std::string_view sql, bool sync_wait) {
    if (in_replay_ || !wal_.is_open()) return;
    if (in_txn_.load(std::memory_order_acquire)) {
        txn_wal_buffer_.emplace_back(sql);
        return;
    }
    uint64_t lsn = wal_.append(1, sql, sync_wait);
    if (replicate_cb_ && lsn > 0) {
        replicate_cb_(lsn, sql);
    }
}

ZephyrResultSet* ZephyrEngine::exec_stmt(
    const Stmt& stmt,
    std::string_view raw_sql,
    const std::vector<Value>& params,
    bool log_wal,
    bool sync_wal
) {
    return std::visit([&](const auto& s) -> ZephyrResultSet* {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, CreateTableStmt>) {
            auto* r = exec_create_table(s);
            if (r->status == ZEPHYR_OK && log_wal) log_wal_entry(raw_sql, sync_wal);
            return r;
        } else if constexpr (std::is_same_v<T, DropTableStmt>) {
            auto* r = exec_drop_table(s);
            if (r->status == ZEPHYR_OK && log_wal) log_wal_entry(raw_sql, sync_wal);
            return r;
        } else if constexpr (std::is_same_v<T, InsertStmt>) {
            auto* r = exec_insert(s, params);
            if (r->status == ZEPHYR_OK && log_wal && params.empty()) log_wal_entry(raw_sql, sync_wal);
            return r;
        } else if constexpr (std::is_same_v<T, UpdateStmt>) {
            auto* r = exec_update(s, params);
            if (r->status == ZEPHYR_OK && log_wal && params.empty()) log_wal_entry(raw_sql, sync_wal);
            return r;
        } else if constexpr (std::is_same_v<T, DeleteStmt>) {
            auto* r = exec_delete(s, params);
            if (r->status == ZEPHYR_OK && log_wal && params.empty()) log_wal_entry(raw_sql, sync_wal);
            return r;
        } else if constexpr (std::is_same_v<T, SelectStmt>) {
            return exec_select(s, params, nullptr);
        } else if constexpr (std::is_same_v<T, TxnStmt>) {
            return exec_txn(s);
        } else {
            return make_error_result(ZEPHYR_ERR_INTERNAL, "unsupported statement");
        }
    }, stmt);
}

ZephyrResultSet* ZephyrEngine::exec_txn(const TxnStmt& s) {
    if (s.kind == TxnStmt::Kind::Begin) {
        if (in_txn_) {
            return make_error_result(ZEPHYR_ERR_INVALID_ARG, "transaction already active");
        }
        in_txn_.store(true, std::memory_order_release);
        txn_backup_.clear();
        for (const auto& [k, tbl] : tables_) {
            std::unique_lock<std::shared_mutex> row_lk(tbl->rw_mu);
            AllShardsLock shards_lk(*tbl);
            txn_backup_[k] = tbl->clone_data(&reaper_);
        }
        txn_wal_buffer_.clear();
        active_txns_.fetch_add(1, std::memory_order_relaxed);
        return pack_result({}, {}, {}, 0, 0);
    }
    if (s.kind == TxnStmt::Kind::Commit) {
        if (!in_txn_) {
            return pack_result({}, {}, {}, 0, 0);
        }
        in_txn_.store(false, std::memory_order_release);
        active_txns_.fetch_sub(1, std::memory_order_relaxed);
        txn_backup_.clear();
        if (!in_replay_ && wal_.is_open()) {
            for (const auto& sql : txn_wal_buffer_) {
                uint64_t lsn = wal_.append(1, sql, false);
                if (replicate_cb_ && lsn > 0) replicate_cb_(lsn, sql);
            }
            wal_.flush();
        }
        txn_wal_buffer_.clear();
        return pack_result({}, {}, {}, 0, 0);
    }
    // Rollback
    if (!in_txn_) {
        return pack_result({}, {}, {}, 0, 0);
    }
    auto old_tables = std::move(tables_);
    tables_ = std::move(txn_backup_);
    for (auto& [k, tbl] : tables_) {
        rebuild_btree(*tbl);
    }
    publish_catalog_snapshot();
    for (auto& [k, old_tbl] : old_tables) {
        if (old_tbl) {
            reaper_.retire_obj(old_tbl.release());
        }
    }
    txn_backup_.clear();
    txn_wal_buffer_.clear();
    in_txn_.store(false, std::memory_order_release);
    active_txns_.fetch_sub(1, std::memory_order_relaxed);
    return pack_result({}, {}, {}, 0, 0);
}

ZephyrResultSet* ZephyrEngine::exec_create_table(const CreateTableStmt& s) {
    std::string key = to_lower(s.table_name);
    if (tables_.count(key)) {
        if (s.if_not_exists) {
            return pack_result({}, {}, {}, 0, 0);
        }
        return make_error_result(ZEPHYR_ERR_CONSTRAINT, "table already exists: " + s.table_name);
    }

    auto tbl = std::make_unique<Table>();
    tbl->name = s.table_name;
    tbl->btree = std::make_unique<zephyr::core::ShardedAugmentedBTree>(&reaper_);

    int agg_slot = 0;
    for (size_t i = 0; i < s.columns.size(); ++i) {
        const auto& cd = s.columns[i];
        ColumnSchema cs;
        cs.name = cd.name;
        cs.type = cd.type;
        cs.primary_key = cd.primary_key;
        cs.not_null = cd.not_null || cd.primary_key;
        if ((cd.type == ColumnType::I64 || cd.type == ColumnType::F64) &&
            agg_slot < static_cast<int>(zephyr::core::AugmentedBTree::MAX_AGG_COLS)) {
            cs.agg_slot = agg_slot++;
        }
        if (cs.primary_key && tbl->pk_col < 0) {
            tbl->pk_col = static_cast<int>(i);
            tbl->pk_is_i64 = (cs.type == ColumnType::I64);
        }
        tbl->col_map[to_lower(cd.name)] = i;
        tbl->schema.push_back(std::move(cs));

        ColumnData col_data;
        col_data.type = cd.type;
        tbl->columns.push_back(std::move(col_data));
    }

    tables_[key] = std::move(tbl);
    publish_catalog_snapshot();
    return pack_result({}, {}, {}, 0, 0);
}

ZephyrResultSet* ZephyrEngine::exec_drop_table(const DropTableStmt& s) {
    std::string key = to_lower(s.table_name);
    auto it = tables_.find(key);
    if (it == tables_.end()) {
        if (s.if_exists) {
            return pack_result({}, {}, {}, 0, 0);
        }
        return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.table_name);
    }
    std::unique_ptr<Table> dropped = std::move(it->second);
    tables_.erase(it);
    publish_catalog_snapshot();
    if (dropped) {
        reaper_.retire_obj(dropped.release());
    }
    return pack_result({}, {}, {}, 0, 0);
}

void ZephyrEngine::update_btree_for_row(Table& tbl, size_t row_idx) {
    std::array<int64_t, zephyr::core::AugmentedBTree::MAX_AGG_COLS> i64_vals{};
    std::array<double, zephyr::core::AugmentedBTree::MAX_AGG_COLS> f64_vals{};
    for (size_t c = 0; c < tbl.schema.size(); ++c) {
        int slot = tbl.schema[c].agg_slot;
        if (slot < 0 || tbl.columns[c].nulls[row_idx]) continue;
        if (tbl.schema[c].type == ColumnType::I64) {
            i64_vals[slot] = tbl.columns[c].i64_data[row_idx];
        } else if (tbl.schema[c].type == ColumnType::F64) {
            f64_vals[slot] = tbl.columns[c].f64_data[row_idx];
        }
    }
    int64_t key = tbl.pk_is_i64
        ? tbl.columns[tbl.pk_col].i64_data[row_idx]
        : static_cast<int64_t>(row_idx + 1);
    tbl.btree->upsert(key, static_cast<uint64_t>(row_idx), i64_vals, f64_vals);
}

void ZephyrEngine::rebuild_btree(Table& tbl) {
    tbl.btree->clear();
    for (size_t r = 0; r < tbl.live.size(); ++r) {
        if (tbl.live[r]) {
            update_btree_for_row(tbl, r);
        }
    }
}

ZephyrResultSet* ZephyrEngine::exec_insert(const InsertStmt& s, const std::vector<Value>& params) {
    auto it = tables_.find(to_lower(s.table_name));
    if (it == tables_.end()) {
        return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.table_name);
    }
    Table& tbl = *it->second;

    std::vector<size_t> target_cols;
    if (s.columns.empty()) {
        for (size_t i = 0; i < tbl.schema.size(); ++i) target_cols.push_back(i);
    } else {
        for (const auto& cname : s.columns) {
            int idx = tbl.find_col(cname);
            if (idx < 0) {
                return make_error_result(ZEPHYR_ERR_NOT_FOUND, "unknown column: " + cname);
            }
            target_cols.push_back(static_cast<size_t>(idx));
        }
    }

    std::vector<std::string> all_col_names;
    for (const auto& cs : tbl.schema) all_col_names.push_back(cs.name);

    std::vector<std::vector<Value>> ret_rows;
    EvalContext empty_ctx{nullptr, nullptr, nullptr, &params};

    for (const auto& expr_row : s.rows) {
        if (expr_row.size() != target_cols.size()) {
            return make_error_result(ZEPHYR_ERR_INVALID_ARG, "INSERT column count mismatch");
        }
        std::vector<Value> full_row(tbl.schema.size(), Value::make_null());
        for (size_t j = 0; j < target_cols.size(); ++j) {
            Value raw = eval_expr(*expr_row[j], empty_ctx);
            std::string err;
            Value coerced = coerce_value(raw, tbl.schema[target_cols[j]].type, err);
            if (!err.empty()) {
                return make_error_result(ZEPHYR_ERR_INVALID_ARG, err);
            }
            full_row[target_cols[j]] = std::move(coerced);
        }

        // Check NOT NULL and PRIMARY KEY constraints
        for (size_t c = 0; c < tbl.schema.size(); ++c) {
            if (tbl.schema[c].not_null && full_row[c].is_null()) {
                return make_error_result(
                    ZEPHYR_ERR_CONSTRAINT, "NULL value violates NOT NULL on " + tbl.schema[c].name);
            }
        }
        if (tbl.pk_col >= 0) {
            const Value& pk_val = full_row[tbl.pk_col];
            if (tbl.pk_is_i64) {
                const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val.i64);
                if (tbl.pk_shard_index[shard].count(pk_val.i64)) {
                    return make_error_result(ZEPHYR_ERR_CONSTRAINT, "duplicate primary key");
                }
            } else {
                if (tbl.pk_str_index.count(value_key_string(pk_val))) {
                    return make_error_result(ZEPHYR_ERR_CONSTRAINT, "duplicate primary key");
                }
            }
        }

        size_t row_idx = tbl.live.size();
        for (size_t c = 0; c < tbl.schema.size(); ++c) {
            tbl.columns[c].push_value(full_row[c]);
        }
        tbl.live.push_back(1);

        if (tbl.pk_col >= 0) {
            const Value& pk_val = full_row[tbl.pk_col];
            if (tbl.pk_is_i64) {
                const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk_val.i64);
                tbl.pk_shard_index[shard][pk_val.i64] = row_idx;
            } else {
                tbl.pk_str_index[value_key_string(pk_val)] = row_idx;
            }
        }
        update_btree_for_row(tbl, row_idx);

        if (!s.returning.empty()) {
            ret_rows.push_back(std::move(full_row));
        }
    }

    uint64_t affected = s.rows.size();
    if (s.returning.empty()) {
        return pack_result({}, {}, {}, affected, 0);
    }

    std::vector<std::string> out_names;
    std::vector<std::string> out_qualifiers;
    std::vector<ColumnType> out_types;
    std::vector<std::vector<Value>> out_rows;
    for (size_t r = 0; r < ret_rows.size(); ++r) {
        EvalContext rctx{&all_col_names, nullptr, &ret_rows[r], &params};
        std::vector<Value> proj;
        for (size_t i = 0; i < s.returning.size(); ++i) {
            const auto& item = s.returning[i];
            if (item.is_star) {
                for (size_t c = 0; c < tbl.schema.size(); ++c) {
                    if (r == 0) {
                        out_names.push_back(tbl.schema[c].name);
                        out_types.push_back(tbl.schema[c].type);
                    }
                    proj.push_back(ret_rows[r][c]);
                }
            } else {
                Value v = eval_expr(*item.expr, rctx);
                if (r == 0) {
                    out_names.push_back(item.alias.empty() ? infer_expr_name(item.expr.get(), i) : item.alias);
                    out_types.push_back(v.type);
                }
                proj.push_back(std::move(v));
            }
        }
        out_rows.push_back(std::move(proj));
    }
    return pack_result(out_names, out_types, out_rows, affected, ZEPHYR_FLAG_RETURNING);
}

std::vector<uint32_t> ZephyrEngine::scan_matching_rows(
    const Table& tbl,
    const Expr* where,
    const std::vector<Value>& params
) {
    // Fast SIMD path for simple `col <op> literal/param` on I64 or F64 columns
    if (where && std::holds_alternative<BinaryExpr>(where->node)) {
        const auto& bin = std::get<BinaryExpr>(where->node);
        const ColumnRefExpr* col_ref = nullptr;
        const Expr* rhs_expr = nullptr;
        bool flip = false;

        if (std::holds_alternative<ColumnRefExpr>(bin.left->node) &&
            (std::holds_alternative<LiteralExpr>(bin.right->node) ||
             std::holds_alternative<ParamExpr>(bin.right->node))) {
            col_ref = &std::get<ColumnRefExpr>(bin.left->node);
            rhs_expr = bin.right.get();
        } else if (std::holds_alternative<ColumnRefExpr>(bin.right->node) &&
                   (std::holds_alternative<LiteralExpr>(bin.left->node) ||
                    std::holds_alternative<ParamExpr>(bin.left->node))) {
            col_ref = &std::get<ColumnRefExpr>(bin.right->node);
            rhs_expr = bin.left.get();
            flip = true;
        }

        auto cmp = to_cmp_op(bin.op, flip);
        if (col_ref && rhs_expr && cmp.has_value()) {
            int col_idx = tbl.find_col(col_ref->column);
            if (col_idx >= 0) {
                EvalContext empty_ctx{nullptr, nullptr, nullptr, &params};
                Value rhs_val = eval_expr(*rhs_expr, empty_ctx);
                const auto& cdata = tbl.columns[col_idx];
                if (!rhs_val.is_null()) {
                    if (cdata.type == ColumnType::I64 &&
                        (rhs_val.type == ColumnType::I64 || rhs_val.type == ColumnType::F64)) {
                        int64_t v = (rhs_val.type == ColumnType::I64)
                            ? rhs_val.i64
                            : static_cast<int64_t>(rhs_val.f64);
                        return filter_column_i64(
                            cdata.i64_data.data(), cdata.i64_data.size(), v, *cmp,
                            cdata.nulls.data(), tbl.live.data());
                    }
                    if (cdata.type == ColumnType::F64 &&
                        (rhs_val.type == ColumnType::F64 || rhs_val.type == ColumnType::I64)) {
                        double v = (rhs_val.type == ColumnType::F64)
                            ? rhs_val.f64
                            : static_cast<double>(rhs_val.i64);
                        return filter_column_f64(
                            cdata.f64_data.data(), cdata.f64_data.size(), v, *cmp,
                            cdata.nulls.data(), tbl.live.data());
                    }
                }
            }
        }
    }

    // PK point lookup fastpath: WHERE pk = literal/param
    if (where && tbl.pk_col >= 0 && std::holds_alternative<BinaryExpr>(where->node)) {
        const auto& bin = std::get<BinaryExpr>(where->node);
        if (bin.op == BinaryOp::Eq) {
            const ColumnRefExpr* col_ref = nullptr;
            const Expr* rhs_expr = nullptr;
            if (std::holds_alternative<ColumnRefExpr>(bin.left->node)) {
                col_ref = &std::get<ColumnRefExpr>(bin.left->node);
                rhs_expr = bin.right.get();
            } else if (std::holds_alternative<ColumnRefExpr>(bin.right->node)) {
                col_ref = &std::get<ColumnRefExpr>(bin.right->node);
                rhs_expr = bin.left.get();
            }
            if (col_ref && rhs_expr && tbl.find_col(col_ref->column) == tbl.pk_col) {
                EvalContext empty_ctx{nullptr, nullptr, nullptr, &params};
                Value rhs_val = eval_expr(*rhs_expr, empty_ctx);
                if (!rhs_val.is_null()) {
                    if (tbl.pk_is_i64 && rhs_val.type == ColumnType::I64) {
                        uint64_t rid = 0;
                        if (tbl.btree->find(rhs_val.i64, rid)) {
                            return {static_cast<uint32_t>(rid)};
                        }
                        return {};
                    }
                }
            }
        }
    }

    std::vector<uint32_t> matched;
    std::vector<std::string> col_names;
    col_names.reserve(tbl.schema.size());
    for (const auto& cs : tbl.schema) col_names.push_back(cs.name);

    std::vector<Value> row_buf(tbl.schema.size());
    EvalContext ctx{&col_names, nullptr, &row_buf, &params};

    for (size_t r = 0; r < tbl.live.size(); ++r) {
        if (!tbl.live[r]) continue;
        if (!where) {
            matched.push_back(static_cast<uint32_t>(r));
            continue;
        }
        for (size_t c = 0; c < tbl.schema.size(); ++c) {
            row_buf[c] = tbl.columns[c].get_value(r);
        }
        if (truthy(eval_expr(*where, ctx))) {
            matched.push_back(static_cast<uint32_t>(r));
        }
    }
    return matched;
}

ZephyrResultSet* ZephyrEngine::exec_update(const UpdateStmt& s, const std::vector<Value>& params) {
    auto it = tables_.find(to_lower(s.table_name));
    if (it == tables_.end()) {
        return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.table_name);
    }
    Table& tbl = *it->second;

    std::vector<size_t> assign_cols;
    for (const auto& a : s.assignments) {
        int idx = tbl.find_col(a.column);
        if (idx < 0) {
            return make_error_result(ZEPHYR_ERR_NOT_FOUND, "unknown column: " + a.column);
        }
        assign_cols.push_back(static_cast<size_t>(idx));
    }

    auto matched = scan_matching_rows(tbl, s.where.get(), params);
    std::vector<std::string> col_names;
    for (const auto& cs : tbl.schema) col_names.push_back(cs.name);

    std::vector<std::vector<Value>> ret_rows;
    for (uint32_t r : matched) {
        std::vector<Value> row_vals(tbl.schema.size());
        for (size_t c = 0; c < tbl.schema.size(); ++c) {
            row_vals[c] = tbl.columns[c].get_value(r);
        }
        EvalContext ctx{&col_names, nullptr, &row_vals, &params};

        for (size_t j = 0; j < s.assignments.size(); ++j) {
            size_t cidx = assign_cols[j];
            Value v = eval_expr(*s.assignments[j].value, ctx);
            std::string err;
            Value coerced = coerce_value(v, tbl.schema[cidx].type, err);
            if (!err.empty()) {
                return make_error_result(ZEPHYR_ERR_INVALID_ARG, err);
            }
            if (tbl.schema[cidx].not_null && coerced.is_null()) {
                return make_error_result(ZEPHYR_ERR_CONSTRAINT, "NULL violates NOT NULL");
            }
            if (static_cast<int>(cidx) == tbl.pk_col) {
                if (tbl.pk_is_i64) {
                    int64_t old_pk = tbl.columns[cidx].i64_data[r];
                    const size_t old_shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(old_pk);
                    const size_t new_shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(coerced.i64);
                    tbl.pk_shard_index[old_shard].erase(old_pk);
                    tbl.btree->erase(old_pk);
                    tbl.pk_shard_index[new_shard][coerced.i64] = r;
                } else {
                    tbl.pk_str_index.erase(value_key_string(tbl.columns[cidx].get_value(r)));
                    tbl.pk_str_index[value_key_string(coerced)] = r;
                }
            }
            tbl.columns[cidx].set_value(r, coerced);
            row_vals[cidx] = std::move(coerced);
        }
        update_btree_for_row(tbl, r);
        if (!s.returning.empty()) {
            ret_rows.push_back(std::move(row_vals));
        }
    }

    uint64_t affected = matched.size();
    if (s.returning.empty()) {
        return pack_result({}, {}, {}, affected, 0);
    }

    std::vector<std::string> out_names;
    std::vector<std::string> out_qualifiers;
    std::vector<ColumnType> out_types;
    std::vector<std::vector<Value>> out_rows;
    for (size_t r = 0; r < ret_rows.size(); ++r) {
        EvalContext rctx{&col_names, nullptr, &ret_rows[r], &params};
        std::vector<Value> proj;
        for (size_t i = 0; i < s.returning.size(); ++i) {
            const auto& item = s.returning[i];
            if (item.is_star) {
                for (size_t c = 0; c < tbl.schema.size(); ++c) {
                    if (r == 0) {
                        out_names.push_back(tbl.schema[c].name);
                        out_types.push_back(tbl.schema[c].type);
                    }
                    proj.push_back(ret_rows[r][c]);
                }
            } else {
                Value v = eval_expr(*item.expr, rctx);
                if (r == 0) {
                    out_names.push_back(item.alias.empty() ? infer_expr_name(item.expr.get(), i) : item.alias);
                    out_types.push_back(v.type);
                }
                proj.push_back(std::move(v));
            }
        }
        out_rows.push_back(std::move(proj));
    }
    return pack_result(out_names, out_types, out_rows, affected, ZEPHYR_FLAG_RETURNING);
}

ZephyrResultSet* ZephyrEngine::exec_delete(const DeleteStmt& s, const std::vector<Value>& params) {
    auto it = tables_.find(to_lower(s.table_name));
    if (it == tables_.end()) {
        return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.table_name);
    }
    Table& tbl = *it->second;

    auto matched = scan_matching_rows(tbl, s.where.get(), params);
    std::vector<std::string> col_names;
    for (const auto& cs : tbl.schema) col_names.push_back(cs.name);

    std::vector<std::vector<Value>> ret_rows;
    for (uint32_t r : matched) {
        if (!s.returning.empty()) {
            std::vector<Value> row_vals(tbl.schema.size());
            for (size_t c = 0; c < tbl.schema.size(); ++c) {
                row_vals[c] = tbl.columns[c].get_value(r);
            }
            ret_rows.push_back(std::move(row_vals));
        }
        if (tbl.pk_col >= 0) {
            if (tbl.pk_is_i64) {
                int64_t pk = tbl.columns[tbl.pk_col].i64_data[r];
                const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(pk);
                tbl.pk_shard_index[shard].erase(pk);
                tbl.btree->erase(pk);
            } else {
                tbl.pk_str_index.erase(value_key_string(tbl.columns[tbl.pk_col].get_value(r)));
                tbl.btree->erase(static_cast<int64_t>(r + 1));
            }
        } else {
            tbl.btree->erase(static_cast<int64_t>(r + 1));
        }
        tbl.live[r] = 0;
    }

    uint64_t affected = matched.size();
    if (s.returning.empty()) {
        return pack_result({}, {}, {}, affected, 0);
    }

    std::vector<std::string> out_names;
    std::vector<std::string> out_qualifiers;
    std::vector<ColumnType> out_types;
    std::vector<std::vector<Value>> out_rows;
    for (size_t r = 0; r < ret_rows.size(); ++r) {
        EvalContext rctx{&col_names, nullptr, &ret_rows[r], &params};
        std::vector<Value> proj;
        for (size_t i = 0; i < s.returning.size(); ++i) {
            const auto& item = s.returning[i];
            if (item.is_star) {
                for (size_t c = 0; c < tbl.schema.size(); ++c) {
                    if (r == 0) {
                        out_names.push_back(tbl.schema[c].name);
                        out_types.push_back(tbl.schema[c].type);
                    }
                    proj.push_back(ret_rows[r][c]);
                }
            } else {
                Value v = eval_expr(*item.expr, rctx);
                if (r == 0) {
                    out_names.push_back(item.alias.empty() ? infer_expr_name(item.expr.get(), i) : item.alias);
                    out_types.push_back(v.type);
                }
                proj.push_back(std::move(v));
            }
        }
        out_rows.push_back(std::move(proj));
    }
    return pack_result(out_names, out_types, out_rows, affected, ZEPHYR_FLAG_RETURNING);
}

Table* ZephyrEngine::resolve_table(
    std::string_view name,
    const std::unordered_map<std::string, std::unique_ptr<Table>>* cte_scope
) {
    std::string key = to_lower(name);
    if (cte_scope) {
        auto it = cte_scope->find(key);
        if (it != cte_scope->end()) return it->second.get();
    }
    auto it = tables_.find(key);
    return it != tables_.end() ? it->second.get() : nullptr;
}

std::unique_ptr<Table> ZephyrEngine::build_virtual_table(std::string_view lower_name) const {
    auto add_col = [](Table& t, const char* name, ColumnType ty) {
        size_t idx = t.schema.size();
        ColumnSchema cs;
        cs.name = name;
        cs.type = ty;
        t.col_map[to_lower(name)] = idx;
        t.schema.push_back(std::move(cs));
        ColumnData cd;
        cd.type = ty;
        t.columns.push_back(std::move(cd));
    };

    // Sort user tables by name for deterministic metadata output
    std::vector<const Table*> sorted_tables;
    sorted_tables.reserve(tables_.size());
    for (const auto& [k, tbl] : tables_) {
        if (tbl) sorted_tables.push_back(tbl.get());
    }
    std::sort(sorted_tables.begin(), sorted_tables.end(),
              [](const Table* a, const Table* b) { return a->name < b->name; });

    if (lower_name == "information_schema.tables" || lower_name == "pg_catalog.pg_tables") {
        auto vt = std::make_unique<Table>();
        vt->name = std::string(lower_name);
        add_col(*vt, "table_schema", ColumnType::Text);
        add_col(*vt, "table_name", ColumnType::Text);
        add_col(*vt, "table_type", ColumnType::Text);
        add_col(*vt, "row_count", ColumnType::I64);

        for (const Table* tbl : sorted_tables) {
            int64_t rcnt = tbl->btree ? static_cast<int64_t>(tbl->btree->total_count()) : 0;
            vt->columns[0].push_value(Value::make_text("public"));
            vt->columns[1].push_value(Value::make_text(tbl->name));
            vt->columns[2].push_value(Value::make_text("BASE TABLE"));
            vt->columns[3].push_value(Value::make_i64(rcnt));
            vt->live.push_back(1);
        }
        return vt;
    }

    if (lower_name == "information_schema.columns") {
        auto vt = std::make_unique<Table>();
        vt->name = "information_schema.columns";
        add_col(*vt, "table_name", ColumnType::Text);
        add_col(*vt, "column_name", ColumnType::Text);
        add_col(*vt, "ordinal_position", ColumnType::I64);
        add_col(*vt, "data_type", ColumnType::Text);
        add_col(*vt, "is_nullable", ColumnType::Text);
        add_col(*vt, "is_primary_key", ColumnType::Bool);

        for (const Table* tbl : sorted_tables) {
            for (size_t c = 0; c < tbl->schema.size(); ++c) {
                const ColumnSchema& cs = tbl->schema[c];
                vt->columns[0].push_value(Value::make_text(tbl->name));
                vt->columns[1].push_value(Value::make_text(cs.name));
                vt->columns[2].push_value(Value::make_i64(static_cast<int64_t>(c + 1)));
                vt->columns[3].push_value(Value::make_text(column_type_sql_name(cs.type)));
                vt->columns[4].push_value(Value::make_text(cs.not_null ? "NO" : "YES"));
                vt->columns[5].push_value(Value::make_bool(cs.primary_key));
                vt->live.push_back(1);
            }
        }
        return vt;
    }

    return nullptr;
}

// ============================================================================
// SELECT Execution (O(1) B-Tree Fastpath, SIMD Scan, Hash Join, CTEs, GroupBy)
// ============================================================================

ZephyrResultSet* ZephyrEngine::exec_select(
    const SelectStmt& s,
    const std::vector<Value>& params,
    const std::unordered_map<std::string, std::unique_ptr<Table>>* outer_ctes
) {
    // 1. Materialize CTEs if present
    std::unordered_map<std::string, std::unique_ptr<Table>> cte_map;
    if (outer_ctes) {
        for (const auto& [k, v] : *outer_ctes) {
            cte_map[k] = v->clone_data(&reaper_);
        }
    }
    for (const auto& cte : s.ctes) {
        ZephyrResultSet* sub = exec_select(*cte.query, params, &cte_map);
        if (sub->status != ZEPHYR_OK) {
            return sub;
        }
        auto tmp = std::make_unique<Table>();
        tmp->name = cte.name;
        tmp->btree = std::make_unique<zephyr::core::ShardedAugmentedBTree>(&reaper_);
        for (uint64_t c = 0; c < sub->col_count; ++c) {
            ColumnSchema cs;
            cs.name = std::string(sub->columns[c].name, sub->columns[c].name_len);
            cs.type = static_cast<ColumnType>(sub->columns[c].type);
            tmp->col_map[to_lower(cs.name)] = c;
            tmp->schema.push_back(cs);
            ColumnData cd;
            cd.type = cs.type;
            tmp->columns.push_back(std::move(cd));
        }
        for (uint64_t r = 0; r < sub->row_count; ++r) {
            for (uint64_t c = 0; c < sub->col_count; ++c) {
                tmp->columns[c].push_value(from_c_value(sub->values[r * sub->col_count + c]));
            }
            tmp->live.push_back(1);
        }
        free_result(sub);
        cte_map[to_lower(cte.name)] = std::move(tmp);
    }

    // 1.5 Materialize virtual information_schema / pg_catalog system tables if referenced
    auto maybe_add_virtual = [&](std::string_view tname) {
        std::string lower = to_lower(tname);
        if (is_virtual_system_table(lower) && !cte_map.count(lower)) {
            if (auto vt = build_virtual_table(lower)) {
                cte_map[lower] = std::move(vt);
            }
        }
    };
    if (s.from.has_value()) {
        maybe_add_virtual(s.from->table_name);
    }
    for (const auto& jc : s.joins) {
        maybe_add_virtual(jc.table.table_name);
    }

    // 2. O(1) / O(log N) AugmentedBTree Fastpath for single-table aggregates
    if (s.from.has_value() && s.joins.empty() && s.group_by.empty() && !s.items.empty()) {
        Table* tbl = resolve_table(s.from->table_name, &cte_map);
        if (tbl && !cte_map.count(to_lower(s.from->table_name))) {
            bool is_full = !s.where;
            bool is_pk_between = false;
            int64_t bw_lo = 0;
            int64_t bw_hi = 0;

            if (s.where && tbl->pk_is_i64 && std::holds_alternative<BetweenExpr>(s.where->node)) {
                const auto& bw = std::get<BetweenExpr>(s.where->node);
                if (!bw.is_not && std::holds_alternative<ColumnRefExpr>(bw.expr->node)) {
                    const auto& cref = std::get<ColumnRefExpr>(bw.expr->node);
                    if (tbl->find_col(cref.column) == tbl->pk_col) {
                        EvalContext empty_ctx{nullptr, nullptr, nullptr, &params};
                        Value lo = eval_expr(*bw.low, empty_ctx);
                        Value hi = eval_expr(*bw.high, empty_ctx);
                        if (lo.type == ColumnType::I64 && hi.type == ColumnType::I64) {
                            is_pk_between = true;
                            bw_lo = lo.i64;
                            bw_hi = hi.i64;
                        }
                    }
                }
            }

            if (is_full || is_pk_between) {
                std::vector<std::string> fast_names;
                std::vector<ColumnType> fast_types;
                std::vector<Value> fast_vals;
                bool all_fast = true;

                for (size_t i = 0; i < s.items.size(); ++i) {
                    const auto& item = s.items[i];
                    if (item.is_star || !item.expr || !std::holds_alternative<AggregateExpr>(item.expr->node)) {
                        all_fast = false;
                        break;
                    }
                    const auto& agg = std::get<AggregateExpr>(item.expr->node);
                    std::string col_label = item.alias.empty()
                        ? infer_expr_name(item.expr.get(), i) : item.alias;

                    if (agg.func == AggFunc::Count && agg.is_star) {
                        int64_t cnt = is_full
                            ? static_cast<int64_t>(tbl->btree->total_count())
                            : static_cast<int64_t>(tbl->btree->range_count(bw_lo, bw_hi));
                        fast_names.push_back(std::move(col_label));
                        fast_types.push_back(ColumnType::I64);
                        fast_vals.push_back(Value::make_i64(cnt));
                    } else if (agg.func == AggFunc::Sum && agg.arg &&
                               std::holds_alternative<ColumnRefExpr>(agg.arg->node)) {
                        const auto& cref = std::get<ColumnRefExpr>(agg.arg->node);
                        int cidx = tbl->find_col(cref.column);
                        if (cidx >= 0 && tbl->schema[cidx].agg_slot >= 0) {
                            size_t slot = static_cast<size_t>(tbl->schema[cidx].agg_slot);
                            if (tbl->schema[cidx].type == ColumnType::I64) {
                                int64_t sum = is_full
                                    ? tbl->btree->total_sum_i64(slot)
                                    : tbl->btree->range_sum_i64(bw_lo, bw_hi, slot);
                                fast_names.push_back(std::move(col_label));
                                fast_types.push_back(ColumnType::I64);
                                fast_vals.push_back(Value::make_i64(sum));
                            } else if (tbl->schema[cidx].type == ColumnType::F64) {
                                double sum = is_full
                                    ? tbl->btree->total_sum_f64(slot)
                                    : tbl->btree->range_sum_f64(bw_lo, bw_hi, slot);
                                fast_names.push_back(std::move(col_label));
                                fast_types.push_back(ColumnType::F64);
                                fast_vals.push_back(Value::make_f64(sum));
                            } else {
                                all_fast = false;
                                break;
                            }
                        } else {
                            all_fast = false;
                            break;
                        }
                    } else {
                        all_fast = false;
                        break;
                    }
                }

                if (all_fast) {
                    stats_slots_[stat_slot()].o1_hits.fetch_add(1, std::memory_order_relaxed);
                    return pack_result(fast_names, fast_types, {std::move(fast_vals)}, 0, ZEPHYR_FLAG_O1_FASTPATH);
                }
            }
        }
    }

    // 2.5 Direct-to-Arena Fast Projection for simple single-table SELECT
    if (cte_map.empty() && s.from.has_value() && s.joins.empty() &&
        s.group_by.empty() && !s.distinct && s.order_by.empty() && !s.items.empty()) {
        Table* base = resolve_table(s.from->table_name, nullptr);
        if (!base) {
            return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.from->table_name);
        }
        std::string base_qual = s.from->alias.empty() ? base->name : s.from->alias;
        std::string base_qual_lower = to_lower(base_qual);

        bool can_fast_project = true;
        std::vector<int> proj_cols;
        std::vector<std::string> fast_names;
        std::vector<ColumnType> fast_types;

        for (size_t i = 0; i < s.items.size(); ++i) {
            const auto& item = s.items[i];
            if (item.is_star) {
                if (!item.table_qualifier.empty() && to_lower(item.table_qualifier) != base_qual_lower) {
                    continue;
                }
                for (size_t c = 0; c < base->schema.size(); ++c) {
                    proj_cols.push_back(static_cast<int>(c));
                    fast_names.push_back(base->schema[c].name);
                    fast_types.push_back(base->schema[c].type);
                }
            } else if (item.expr && std::holds_alternative<ColumnRefExpr>(item.expr->node)) {
                const auto& cref = std::get<ColumnRefExpr>(item.expr->node);
                int cidx = -1;
                if (cref.table.empty() || to_lower(cref.table) == base_qual_lower) {
                    cidx = base->find_col(cref.column);
                }
                proj_cols.push_back(cidx);
                fast_names.push_back(item.alias.empty() ? cref.column : item.alias);
                fast_types.push_back(cidx >= 0 ? base->schema[cidx].type : ColumnType::Null);
            } else {
                can_fast_project = false;
                break;
            }
        }

        if (can_fast_project) {
            std::shared_lock<std::shared_mutex> row_lk(base->rw_mu);
            auto matched = scan_matching_rows(*base, s.where.get(), params);
            size_t start_idx = 0;
            if (s.offset.has_value() && *s.offset > 0) {
                start_idx = std::min(matched.size(), static_cast<size_t>(*s.offset));
            }
            size_t end_idx = matched.size();
            if (s.limit.has_value() && *s.limit >= 0) {
                end_idx = std::min(end_idx, start_idx + static_cast<size_t>(*s.limit));
            }
            size_t row_count = end_idx - start_idx;
            size_t col_count = fast_names.size();

            auto* arena = acquire_arena(static_cast<size_t>(config_.arena_slab_bytes));
            auto* rs = arena->create<ZephyrResultSet>();
            rs->_arena_handle = arena;
            rs->status = ZEPHYR_OK;
            rs->flags = 0;
            rs->rows_affected = 0;
            rs->col_count = col_count;
            rs->row_count = row_count;

            if (col_count > 0) {
                auto* cols = arena->alloc_array<ZephyrColumnDesc>(col_count);
                for (size_t c = 0; c < col_count; ++c) {
                    cols[c].name = arena->dup_cstr(fast_names[c]);
                    cols[c].name_len = static_cast<uint32_t>(fast_names[c].size());
                    cols[c].type = static_cast<int32_t>(fast_types[c]);
                }
                rs->columns = cols;
            } else {
                rs->columns = nullptr;
            }

            if (row_count > 0 && col_count > 0) {
                auto* vals = arena->alloc_array<ZephyrValue>(row_count * col_count);
                for (size_t r_off = 0; r_off < row_count; ++r_off) {
                    uint32_t r_idx = matched[start_idx + r_off];
                    for (size_t c = 0; c < col_count; ++c) {
                        ZephyrValue& zv = vals[r_off * col_count + c];
                        int src_col = proj_cols[c];
                        if (src_col < 0) {
                            zv.type = ZEPHYR_TYPE_NULL;
                            zv._pad = 0;
                            zv.as.i64 = 0;
                            continue;
                        }
                        const ColumnData& cdata = base->columns[src_col];
                        if (cdata.nulls[r_idx]) {
                            zv.type = ZEPHYR_TYPE_NULL;
                            zv._pad = 0;
                            zv.as.i64 = 0;
                            continue;
                        }
                        zv._pad = 0;
                        switch (cdata.type) {
                            case ColumnType::I64:
                                zv.type = ZEPHYR_TYPE_I64;
                                zv.as.i64 = cdata.i64_data[r_idx];
                                break;
                            case ColumnType::F64:
                                zv.type = ZEPHYR_TYPE_F64;
                                zv.as.f64 = cdata.f64_data[r_idx];
                                break;
                            case ColumnType::Bool:
                                zv.type = ZEPHYR_TYPE_BOOL;
                                zv.as.boolean = cdata.bool_data[r_idx] ? 1 : 0;
                                break;
                            case ColumnType::Text: {
                                const std::string& sv = cdata.str_data[r_idx];
                                zv.type = ZEPHYR_TYPE_TEXT;
                                zv.as.slice.len = static_cast<uint32_t>(sv.size());
                                zv.as.slice.reserved = 0;
                                zv.as.slice.ptr = sv.empty()
                                    ? reinterpret_cast<const uint8_t*>("")
                                    : arena->dup_bytes(sv.data(), sv.size());
                                break;
                            }
                            case ColumnType::Bytea: {
                                const std::string& sv = cdata.str_data[r_idx];
                                zv.type = ZEPHYR_TYPE_BYTEA;
                                zv.as.slice.len = static_cast<uint32_t>(sv.size());
                                zv.as.slice.reserved = 0;
                                zv.as.slice.ptr = sv.empty()
                                    ? nullptr
                                    : arena->dup_bytes(sv.data(), sv.size());
                                break;
                            }
                            case ColumnType::Null:
                                zv.type = ZEPHYR_TYPE_NULL;
                                zv.as.i64 = 0;
                                break;
                        }
                    }
                }
                rs->values = vals;
            } else {
                rs->values = nullptr;
            }

            return rs;
        }
    }

    // 3. Build working row set (FROM + JOINs)
    std::vector<std::string> col_names;
    std::vector<std::string> col_qualifiers;
    std::vector<ColumnType> col_types;
    std::vector<std::vector<Value>> rows;

    if (!s.from.has_value()) {
        rows.emplace_back(); // 1 virtual row for SELECT 1+1
    } else {
        Table* base = resolve_table(s.from->table_name, &cte_map);
        if (!base) {
            return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + s.from->table_name);
        }

        // Acquire per-table shared row locks for all real tables referenced in FROM / JOINs
        std::vector<Table*> real_tables;
        if (!cte_map.count(to_lower(s.from->table_name))) {
            real_tables.push_back(base);
        }
        for (const auto& jc : s.joins) {
            if (!cte_map.count(to_lower(jc.table.table_name))) {
                if (Table* jt = resolve_table(jc.table.table_name, &cte_map)) {
                    real_tables.push_back(jt);
                }
            }
        }
        std::sort(real_tables.begin(), real_tables.end());
        real_tables.erase(std::unique(real_tables.begin(), real_tables.end()), real_tables.end());
        std::vector<std::shared_lock<std::shared_mutex>> row_locks;
        row_locks.reserve(real_tables.size());
        for (Table* rt : real_tables) {
            row_locks.emplace_back(rt->rw_mu);
        }

        std::string base_qual = s.from->alias.empty() ? base->name : s.from->alias;
        for (const auto& cs : base->schema) {
            col_names.push_back(cs.name);
            col_qualifiers.push_back(base_qual);
            col_types.push_back(cs.type);
        }

        if (s.joins.empty()) {
            // Single table: use SIMD-accelerated scan_matching_rows
            auto matched = scan_matching_rows(*base, s.where.get(), params);
            rows.reserve(matched.size());
            for (uint32_t r : matched) {
                std::vector<Value> row(base->schema.size());
                for (size_t c = 0; c < base->schema.size(); ++c) {
                    row[c] = base->columns[c].get_value(r);
                }
                rows.push_back(std::move(row));
            }
        } else {
            // Load all live rows of base table
            for (size_t r = 0; r < base->live.size(); ++r) {
                if (!base->live[r]) continue;
                std::vector<Value> row(base->schema.size());
                for (size_t c = 0; c < base->schema.size(); ++c) {
                    row[c] = base->columns[c].get_value(r);
                }
                rows.push_back(std::move(row));
            }

            // Execute each JOIN (hash join when ON is `left_col = right_col`)
            for (const auto& jc : s.joins) {
                Table* rhs = resolve_table(jc.table.table_name, &cte_map);
                if (!rhs) {
                    return make_error_result(ZEPHYR_ERR_NOT_FOUND, "table not found: " + jc.table.table_name);
                }
                std::string rhs_qual = jc.table.alias.empty() ? rhs->name : jc.table.alias;
                size_t left_width = col_names.size();
                size_t right_width = rhs->schema.size();

                std::vector<std::string> new_names = col_names;
                std::vector<std::string> new_quals = col_qualifiers;
                std::vector<ColumnType> new_types = col_types;
                for (const auto& cs : rhs->schema) {
                    new_names.push_back(cs.name);
                    new_quals.push_back(rhs_qual);
                    new_types.push_back(cs.type);
                }

                // Check for equi-join hash optimization
                int left_join_col = -1;
                int right_join_col = -1;
                if (jc.on && std::holds_alternative<BinaryExpr>(jc.on->node)) {
                    const auto& bin = std::get<BinaryExpr>(jc.on->node);
                    if (bin.op == BinaryOp::Eq &&
                        std::holds_alternative<ColumnRefExpr>(bin.left->node) &&
                        std::holds_alternative<ColumnRefExpr>(bin.right->node)) {
                        const auto& c1 = std::get<ColumnRefExpr>(bin.left->node);
                        const auto& c2 = std::get<ColumnRefExpr>(bin.right->node);
                        auto find_left = [&](const ColumnRefExpr& cr) -> int {
                            for (size_t i = 0; i < left_width; ++i) {
                                if (to_lower(col_names[i]) == to_lower(cr.column) &&
                                    (cr.table.empty() || to_lower(col_qualifiers[i]) == to_lower(cr.table))) {
                                    return static_cast<int>(i);
                                }
                            }
                            return -1;
                        };
                        auto find_right = [&](const ColumnRefExpr& cr) -> int {
                            if (!cr.table.empty() && to_lower(cr.table) != to_lower(rhs_qual)) return -1;
                            return rhs->find_col(cr.column);
                        };
                        int l1 = find_left(c1), r2 = find_right(c2);
                        int l2 = find_left(c2), r1 = find_right(c1);
                        if (l1 >= 0 && r2 >= 0) { left_join_col = l1; right_join_col = r2; }
                        else if (l2 >= 0 && r1 >= 0) { left_join_col = l2; right_join_col = r1; }
                    }
                }

                std::vector<std::vector<Value>> joined_rows;
                if (left_join_col >= 0 && right_join_col >= 0) {
                    // Hash join
                    std::unordered_map<std::string, std::vector<size_t>> hash_table;
                    for (size_t r = 0; r < rhs->live.size(); ++r) {
                        if (!rhs->live[r]) continue;
                        Value kv = rhs->columns[right_join_col].get_value(r);
                        if (!kv.is_null()) {
                            hash_table[value_key_string(kv)].push_back(r);
                        }
                    }
                    for (const auto& lrow : rows) {
                        const Value& lv = lrow[left_join_col];
                        bool matched_any = false;
                        if (!lv.is_null()) {
                            auto hit = hash_table.find(value_key_string(lv));
                            if (hit != hash_table.end()) {
                                for (size_t rr : hit->second) {
                                    std::vector<Value> combined = lrow;
                                    combined.reserve(left_width + right_width);
                                    for (size_t c = 0; c < right_width; ++c) {
                                        combined.push_back(rhs->columns[c].get_value(rr));
                                    }
                                    joined_rows.push_back(std::move(combined));
                                    matched_any = true;
                                }
                            }
                        }
                        if (!matched_any && jc.type == JoinType::Left) {
                            std::vector<Value> combined = lrow;
                            combined.resize(left_width + right_width, Value::make_null());
                            joined_rows.push_back(std::move(combined));
                        }
                    }
                } else {
                    // Nested-loop join fallback
                    for (const auto& lrow : rows) {
                        bool matched_any = false;
                        for (size_t rr = 0; rr < rhs->live.size(); ++rr) {
                            if (!rhs->live[rr]) continue;
                            std::vector<Value> combined = lrow;
                            combined.reserve(left_width + right_width);
                            for (size_t c = 0; c < right_width; ++c) {
                                combined.push_back(rhs->columns[c].get_value(rr));
                            }
                            EvalContext jctx{&new_names, &new_quals, &combined, &params};
                            if (truthy(eval_expr(*jc.on, jctx))) {
                                joined_rows.push_back(std::move(combined));
                                matched_any = true;
                            }
                        }
                        if (!matched_any && jc.type == JoinType::Left) {
                            std::vector<Value> combined = lrow;
                            combined.resize(left_width + right_width, Value::make_null());
                            joined_rows.push_back(std::move(combined));
                        }
                    }
                }

                col_names = std::move(new_names);
                col_qualifiers = std::move(new_quals);
                col_types = std::move(new_types);
                rows = std::move(joined_rows);
            }

            // Apply WHERE after JOINs
            if (s.where) {
                std::vector<std::vector<Value>> filtered;
                for (auto& r : rows) {
                    EvalContext wctx{&col_names, &col_qualifiers, &r, &params};
                    if (truthy(eval_expr(*s.where, wctx))) {
                        filtered.push_back(std::move(r));
                    }
                }
                rows = std::move(filtered);
            }
        }
    }

    // 4. Aggregation / GROUP BY
    bool has_agg = !s.group_by.empty();
    if (!has_agg) {
        for (const auto& it : s.items) {
            if (!it.is_star && contains_aggregate(it.expr.get())) {
                has_agg = true;
                break;
            }
        }
    }

    std::vector<std::string> out_names;
    std::vector<std::string> out_qualifiers;
    std::vector<ColumnType> out_types;
    std::vector<std::vector<Value>> out_rows;

    if (has_agg) {
        struct GroupBucket {
            std::vector<Value> sample_row;
            std::vector<size_t> row_indices;
        };
        std::vector<std::string> group_order;
        std::unordered_map<std::string, GroupBucket> groups;

        if (s.group_by.empty()) {
            GroupBucket b;
            for (size_t i = 0; i < rows.size(); ++i) b.row_indices.push_back(i);
            if (!rows.empty()) b.sample_row = rows[0];
            groups[""] = std::move(b);
            group_order.push_back("");
        } else {
            for (size_t r = 0; r < rows.size(); ++r) {
                EvalContext gctx{&col_names, &col_qualifiers, &rows[r], &params};
                std::string gkey;
                for (const auto& ge : s.group_by) {
                    gkey += value_key_string(eval_expr(*ge, gctx));
                    gkey.push_back('\x1f');
                }
                auto git = groups.find(gkey);
                if (git == groups.end()) {
                    GroupBucket b;
                    b.sample_row = rows[r];
                    b.row_indices.push_back(r);
                    groups[gkey] = std::move(b);
                    group_order.push_back(gkey);
                } else {
                    git->second.row_indices.push_back(r);
                }
            }
        }

        auto eval_agg = [&](const AggregateExpr& agg, const std::vector<size_t>& idxs) -> Value {
            if (agg.func == AggFunc::Count && agg.is_star) {
                return Value::make_i64(static_cast<int64_t>(idxs.size()));
            }
            int64_t cnt = 0;
            int64_t sum_i = 0;
            double sum_f = 0.0;
            bool is_float = false;
            Value best = Value::make_null();

            for (size_t r : idxs) {
                EvalContext actx{&col_names, &col_qualifiers, &rows[r], &params};
                Value v = eval_expr(*agg.arg, actx);
                if (v.is_null()) continue;
                ++cnt;
                if (agg.func == AggFunc::Sum || agg.func == AggFunc::Avg) {
                    if (v.type == ColumnType::F64) {
                        if (!is_float) {
                            sum_f = static_cast<double>(sum_i);
                            is_float = true;
                        }
                        sum_f += v.f64;
                    } else if (v.type == ColumnType::I64) {
                        if (is_float) sum_f += static_cast<double>(v.i64);
                        else sum_i += v.i64;
                    }
                } else if (agg.func == AggFunc::Min) {
                    if (best.is_null() || compare_values(v, best) < 0) best = v;
                } else if (agg.func == AggFunc::Max) {
                    if (best.is_null() || compare_values(v, best) > 0) best = v;
                }
            }

            switch (agg.func) {
                case AggFunc::Count: return Value::make_i64(cnt);
                case AggFunc::Sum:
                    if (cnt == 0) return Value::make_null();
                    return is_float ? Value::make_f64(sum_f) : Value::make_i64(sum_i);
                case AggFunc::Avg:
                    if (cnt == 0) return Value::make_null();
                    return Value::make_f64((is_float ? sum_f : static_cast<double>(sum_i)) / static_cast<double>(cnt));
                case AggFunc::Min:
                case AggFunc::Max:
                    return best;
            }
            return Value::make_null();
        };

        std::function<Value(const Expr&, const GroupBucket&)> eval_group_expr;
        eval_group_expr = [&](const Expr& e, const GroupBucket& b) -> Value {
            return std::visit([&](const auto& node) -> Value {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, AggregateExpr>) {
                    return eval_agg(node, b.row_indices);
                } else if constexpr (std::is_same_v<T, UnaryExpr>) {
                    Value v = eval_group_expr(*node.operand, b);
                    if (v.is_null()) return v;
                    if (node.op == UnaryOp::Not) return Value::make_bool(!truthy(v));
                    return v.type == ColumnType::I64 ? Value::make_i64(-v.i64) : Value::make_f64(-v.f64);
                } else if constexpr (std::is_same_v<T, BinaryExpr>) {
                    Value l = eval_group_expr(*node.left, b);
                    Value r = eval_group_expr(*node.right, b);
                    if (l.is_null() || r.is_null()) return Value::make_null();
                    if (node.op == BinaryOp::Add && l.type == ColumnType::I64 && r.type == ColumnType::I64)
                        return Value::make_i64(l.i64 + r.i64);
                    if (node.op == BinaryOp::Sub && l.type == ColumnType::I64 && r.type == ColumnType::I64)
                        return Value::make_i64(l.i64 - r.i64);
                    EvalContext sctx{&col_names, &col_qualifiers, &b.sample_row, &params};
                    return eval_expr(e, sctx);
                } else {
                    if (b.sample_row.empty()) return Value::make_null();
                    EvalContext sctx{&col_names, &col_qualifiers, &b.sample_row, &params};
                    return eval_expr(e, sctx);
                }
            }, e.node);
        };

        for (size_t i = 0; i < s.items.size(); ++i) {
            const auto& it = s.items[i];
            out_names.push_back(it.alias.empty() ? infer_expr_name(it.expr.get(), i) : it.alias);
            std::string qual;
            if (it.expr && std::holds_alternative<ColumnRefExpr>(it.expr->node)) {
                qual = std::get<ColumnRefExpr>(it.expr->node).table;
            }
            out_qualifiers.push_back(std::move(qual));
            out_types.push_back(ColumnType::Null);
        }

        for (const auto& gk : group_order) {
            const auto& bucket = groups[gk];
            std::vector<Value> proj;
            proj.reserve(s.items.size());
            for (size_t i = 0; i < s.items.size(); ++i) {
                Value v = eval_group_expr(*s.items[i].expr, bucket);
                if (out_types[i] == ColumnType::Null && !v.is_null()) {
                    out_types[i] = v.type;
                }
                proj.push_back(std::move(v));
            }
            out_rows.push_back(std::move(proj));
        }
    } else {
        // Non-aggregate projection
        for (size_t i = 0; i < s.items.size(); ++i) {
            const auto& item = s.items[i];
            if (item.is_star) {
                for (size_t c = 0; c < col_names.size(); ++c) {
                    if (item.table_qualifier.empty() ||
                        to_lower(col_qualifiers[c]) == to_lower(item.table_qualifier)) {
                        out_names.push_back(col_names[c]);
                        out_qualifiers.push_back(col_qualifiers[c]);
                        out_types.push_back(col_types[c]);
                    }
                }
            } else {
                out_names.push_back(item.alias.empty() ? infer_expr_name(item.expr.get(), i) : item.alias);
                std::string qual;
                if (item.expr && std::holds_alternative<ColumnRefExpr>(item.expr->node)) {
                    qual = std::get<ColumnRefExpr>(item.expr->node).table;
                }
                out_qualifiers.push_back(std::move(qual));
                out_types.push_back(ColumnType::Null);
            }
        }

        for (size_t r = 0; r < rows.size(); ++r) {
            EvalContext pctx{&col_names, &col_qualifiers, &rows[r], &params};
            std::vector<Value> proj;
            size_t out_c = 0;
            for (const auto& item : s.items) {
                if (item.is_star) {
                    for (size_t c = 0; c < col_names.size(); ++c) {
                        if (item.table_qualifier.empty() ||
                            to_lower(col_qualifiers[c]) == to_lower(item.table_qualifier)) {
                            proj.push_back(rows[r][c]);
                            ++out_c;
                        }
                    }
                } else {
                    Value v = eval_expr(*item.expr, pctx);
                    if (out_types[out_c] == ColumnType::Null && !v.is_null()) {
                        out_types[out_c] = v.type;
                    }
                    proj.push_back(std::move(v));
                    ++out_c;
                }
            }
            out_rows.push_back(std::move(proj));
        }
    }

    // 5. DISTINCT
    if (s.distinct) {
        std::set<std::string> seen;
        std::vector<std::vector<Value>> deduped;
        for (auto& r : out_rows) {
            std::string key;
            for (const auto& v : r) {
                key += value_key_string(v);
                key.push_back('\x1f');
            }
            if (seen.insert(std::move(key)).second) {
                deduped.push_back(std::move(r));
            }
        }
        out_rows = std::move(deduped);
    }

    // 6. ORDER BY
    if (!s.order_by.empty()) {
        std::vector<std::pair<std::vector<Value>, std::vector<Value>>> keyed;
        keyed.reserve(out_rows.size());
        for (size_t r = 0; r < out_rows.size(); ++r) {
            EvalContext octx{&out_names, &out_qualifiers, &out_rows[r], &params};
            std::vector<Value> keys;
            keys.reserve(s.order_by.size());
            for (const auto& ob : s.order_by) {
                Value k = eval_expr(*ob.expr, octx);
                if (k.is_null() && !s.distinct && s.group_by.empty() && r < rows.size()) {
                    EvalContext rctx{&col_names, &col_qualifiers, &rows[r], &params};
                    k = eval_expr(*ob.expr, rctx);
                }
                keys.push_back(std::move(k));
            }
            keyed.emplace_back(std::move(keys), std::move(out_rows[r]));
        }
        std::stable_sort(keyed.begin(), keyed.end(), [&](const auto& a, const auto& b) {
            for (size_t i = 0; i < s.order_by.size(); ++i) {
                int cmp = compare_values(a.first[i], b.first[i]);
                if (cmp != 0) {
                    return s.order_by[i].desc ? (cmp > 0) : (cmp < 0);
                }
            }
            return false;
        });
        out_rows.clear();
        for (auto& kv : keyed) out_rows.push_back(std::move(kv.second));
    }

    // 7. OFFSET & LIMIT
    if (s.offset.has_value() && *s.offset > 0) {
        size_t off = static_cast<size_t>(*s.offset);
        if (off >= out_rows.size()) out_rows.clear();
        else out_rows.erase(out_rows.begin(), out_rows.begin() + static_cast<ptrdiff_t>(off));
    }
    if (s.limit.has_value() && *s.limit >= 0) {
        size_t lim = static_cast<size_t>(*s.limit);
        if (lim < out_rows.size()) out_rows.resize(lim);
    }

    return pack_result(out_names, out_types, out_rows, 0, 0);
}

// ============================================================================
// Snapshot Serialization & MmapStore Loading
// ============================================================================

std::vector<uint8_t> ZephyrEngine::serialize_snapshot() const {
    std::vector<uint8_t> buf;
    // Magic "ZSNP"
    buf.push_back('Z'); buf.push_back('S'); buf.push_back('N'); buf.push_back('P');
    write_u32(buf, 1);
    write_u32(buf, static_cast<uint32_t>(tables_.size()));

    for (const auto& [key, tbl] : tables_) {
        std::shared_lock<std::shared_mutex> row_lk(tbl->rw_mu);
        AllShardsLock shards_lk(*tbl);
        write_str(buf, tbl->name);
        write_u32(buf, static_cast<uint32_t>(tbl->schema.size()));
        for (const auto& cs : tbl->schema) {
            write_str(buf, cs.name);
            write_u8(buf, static_cast<uint8_t>(cs.type));
            write_u8(buf, cs.primary_key ? 1 : 0);
            write_u8(buf, cs.not_null ? 1 : 0);
        }

        uint64_t live_count = 0;
        for (uint8_t l : tbl->live) if (l) ++live_count;
        write_u64(buf, live_count);

        for (size_t r = 0; r < tbl->live.size(); ++r) {
            if (!tbl->live[r]) continue;
            for (size_t c = 0; c < tbl->schema.size(); ++c) {
                Value v = tbl->columns[c].get_value(r);
                write_u8(buf, static_cast<uint8_t>(v.type));
                switch (v.type) {
                    case ColumnType::Null: break;
                    case ColumnType::I64: write_u64(buf, static_cast<uint64_t>(v.i64)); break;
                    case ColumnType::F64: {
                        uint64_t bits = 0;
                        std::memcpy(&bits, &v.f64, sizeof(bits));
                        write_u64(buf, bits);
                        break;
                    }
                    case ColumnType::Bool: write_u8(buf, v.boolean ? 1 : 0); break;
                    case ColumnType::Text:
                    case ColumnType::Bytea: write_str(buf, v.str); break;
                }
            }
        }
    }
    return buf;
}

bool ZephyrEngine::load_snapshot(const std::string& snap_path) {
    if (encrypted_) {
        std::vector<uint8_t> blob;
        bool auth_failed = false;
        if (!mmap_store_.read_snapshot_encrypted(snap_path, snap_key_, blob, &auth_failed)) {
            if (auth_failed) {
                auth_ok_ = false;
                set_error("snapshot authentication failed");
            }
            return false;
        }
        return parse_snapshot_blob(blob.data(), blob.size());
    }

    if (!mmap_store_.open_mmap(snap_path)) return false;
    const uint8_t* p = mmap_store_.data();
    const size_t len = mmap_store_.size();
    if (p && len >= 4 && p[0] == 'Z' && p[1] == 'S' && p[2] == 'N' && p[3] == 'E') {
        auth_ok_ = false;
        set_error("encrypted snapshot requires encryption key");
        return false;
    }
    return parse_snapshot_blob(p, len);
}

bool ZephyrEngine::parse_snapshot_blob(const uint8_t* p, size_t len) {
    const uint8_t* end = p ? (p + len) : nullptr;
    if (!p || len < 12) return false;
    if (p[0] != 'Z' || p[1] != 'S' || p[2] != 'N' || p[3] != 'P') return false;
    p += 4;

    uint32_t version = 0, table_count = 0;
    if (!read_u32(p, end, version) || version != 1) return false;
    if (!read_u32(p, end, table_count)) return false;

    for (uint32_t t = 0; t < table_count; ++t) {
        std::string tname;
        uint32_t col_count = 0;
        if (!read_str(p, end, tname) || !read_u32(p, end, col_count)) return false;

        auto tbl = std::make_unique<Table>();
        tbl->name = tname;
        tbl->btree = std::make_unique<zephyr::core::ShardedAugmentedBTree>(&reaper_);
        int agg_slot = 0;

        for (uint32_t c = 0; c < col_count; ++c) {
            ColumnSchema cs;
            uint8_t ty = 0, pk = 0, nn = 0;
            if (!read_str(p, end, cs.name) || !read_u8(p, end, ty) ||
                !read_u8(p, end, pk) || !read_u8(p, end, nn)) {
                return false;
            }
            cs.type = static_cast<ColumnType>(ty);
            cs.primary_key = pk != 0;
            cs.not_null = nn != 0;
            if ((cs.type == ColumnType::I64 || cs.type == ColumnType::F64) &&
                agg_slot < static_cast<int>(zephyr::core::AugmentedBTree::MAX_AGG_COLS)) {
                cs.agg_slot = agg_slot++;
            }
            if (cs.primary_key && tbl->pk_col < 0) {
                tbl->pk_col = static_cast<int>(c);
                tbl->pk_is_i64 = (cs.type == ColumnType::I64);
            }
            tbl->col_map[to_lower(cs.name)] = c;
            tbl->schema.push_back(std::move(cs));

            ColumnData cd;
            cd.type = tbl->schema.back().type;
            tbl->columns.push_back(std::move(cd));
        }

        uint64_t row_count = 0;
        if (!read_u64(p, end, row_count)) return false;
        for (uint64_t r = 0; r < row_count; ++r) {
            for (uint32_t c = 0; c < col_count; ++c) {
                uint8_t vty = 0;
                if (!read_u8(p, end, vty)) return false;
                ColumnType ct = static_cast<ColumnType>(vty);
                Value v = Value::make_null();
                switch (ct) {
                    case ColumnType::Null: break;
                    case ColumnType::I64: {
                        uint64_t u = 0;
                        if (!read_u64(p, end, u)) return false;
                        v = Value::make_i64(static_cast<int64_t>(u));
                        break;
                    }
                    case ColumnType::F64: {
                        uint64_t bits = 0;
                        if (!read_u64(p, end, bits)) return false;
                        double d = 0.0;
                        std::memcpy(&d, &bits, sizeof(d));
                        v = Value::make_f64(d);
                        break;
                    }
                    case ColumnType::Bool: {
                        uint8_t b = 0;
                        if (!read_u8(p, end, b)) return false;
                        v = Value::make_bool(b != 0);
                        break;
                    }
                    case ColumnType::Text: {
                        std::string s;
                        if (!read_str(p, end, s)) return false;
                        v = Value::make_text(std::move(s));
                        break;
                    }
                    case ColumnType::Bytea: {
                        std::string s;
                        if (!read_str(p, end, s)) return false;
                        v = Value::make_bytea(std::move(s));
                        break;
                    }
                }
                tbl->columns[c].push_value(v);
                if (static_cast<int>(c) == tbl->pk_col && !v.is_null()) {
                    if (tbl->pk_is_i64) {
                        const size_t shard = zephyr::core::ShardedAugmentedBTree::shard_for_key(v.i64);
                        tbl->pk_shard_index[shard][v.i64] = static_cast<size_t>(r);
                    } else {
                        tbl->pk_str_index[value_key_string(v)] = static_cast<size_t>(r);
                    }
                }
            }
            tbl->live.push_back(1);
            update_btree_for_row(*tbl, static_cast<size_t>(r));
        }

        tables_[to_lower(tname)] = std::move(tbl);
    }
    publish_catalog_snapshot();
    return true;
}

} // namespace zephyr::sql
