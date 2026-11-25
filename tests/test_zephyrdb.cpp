#include "zephyr/c_abi.h"
#include "zephyr/compat/cpp26.hpp"
#include "zephyr/core/arena.hpp"
#include "zephyr/core/augmented_btree.hpp"
#include "zephyr/core/rcu.hpp"
#include "zephyr/net/pgwire.hpp"
#include "zephyr/net/raft.hpp"
#include "zephyr/net/studio.hpp"
#include "zephyr/sql/executor.hpp"
#include "zephyr/sql/reflection.hpp"
#include "zephyr/sql/simd_eval.hpp"
#include "zephyr/storage/mmap_store.hpp"
#include "zephyr/storage/wal.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using z_sock_t = SOCKET;
  static constexpr z_sock_t Z_INVALID_SOCK = INVALID_SOCKET;
  static void z_close_sock(z_sock_t s) { closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using z_sock_t = int;
  static constexpr z_sock_t Z_INVALID_SOCK = -1;
  static void z_close_sock(z_sock_t s) { close(s); }
#endif

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << " -> " #cond "\n"; \
            ++g_failures;                                                                \
        }                                                                                \
    } while (false)

struct PlayerStat {
    int64_t id;
    double score;
    bool active;
};
ZEPHYR_REFLECT(PlayerStat, ZEPHYR_FIELD(PlayerStat, id), ZEPHYR_FIELD(PlayerStat, score), ZEPHYR_FIELD(PlayerStat, active))

static void test_arena() {
    std::cout << "Running test_arena...\n";
    zephyr::core::Arena arena(1024);
    void* p1 = arena.alloc_bytes(17, 64);
    CHECK(p1 != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p1) % 64) == 0);

    const char* s = arena.dup_cstr("zephyrdb");
    CHECK(std::strcmp(s, "zephyrdb") == 0);

    // Force slab rollover
    void* p2 = arena.alloc_bytes(4096, 16);
    CHECK(p2 != nullptr);
    CHECK((reinterpret_cast<uintptr_t>(p2) % 16) == 0);
    CHECK(arena.slab_count() >= 2);
    CHECK(arena.bytes_used() >= 4096);

    arena.reset();
    CHECK(arena.bytes_used() == 0);
}

static void test_btree_and_rcu_concurrent() {
    std::cout << "Running test_btree_and_rcu_concurrent...\n";
    zephyr::core::RcuReaper reaper;
    reaper.start(1);
    zephyr::core::AugmentedBTree tree(&reaper);

    constexpr int N = 1000;
    for (int i = 1; i <= N; ++i) {
        std::array<int64_t, 8> i64s{};
        std::array<double, 8> f64s{};
        i64s[0] = i * 10;
        f64s[0] = static_cast<double>(i) * 1.5;
        tree.upsert(i, static_cast<uint64_t>(i), i64s, f64s);
    }

    CHECK(tree.total_count() == N);
    CHECK(tree.total_sum_i64(0) == 10LL * N * (N + 1) / 2);
    CHECK(tree.range_count(100, 199) == 100);
    CHECK(tree.range_sum_i64(1, 10, 0) == 550);

    uint64_t rid = 0;
    CHECK(tree.find(500, rid) && rid == 500);
    CHECK(!tree.find(99999, rid));

    for (int i = 1; i <= 250; ++i) {
        CHECK(tree.erase(i));
    }
    CHECK(tree.total_count() == 750);
    CHECK(!tree.find(100, rid));
    CHECK(tree.find(251, rid) && rid == 251);

    // Multi-threaded RCU stress: 4 lock-free readers + 1 writer
    std::atomic<bool> stop_readers{false};
    std::atomic<uint64_t> reads_done{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            while (!stop_readers.load(std::memory_order_acquire)) {
                auto c = tree.total_count();
                auto s = tree.total_sum_i64(0);
                uint64_t r = 0;
                (void)tree.find(500, r);
                if (c > 0 && s != 0) {
                    reads_done.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int k = 1; k <= 600; ++k) {
        std::array<int64_t, 8> i64s{};
        i64s[0] = k;
        tree.upsert(2000 + k, static_cast<uint64_t>(2000 + k), i64s);
    }
    stop_readers.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();

    CHECK(reads_done.load() > 0);
    CHECK(tree.total_count() == 750 + 600);
    reaper.reclaim_now();
    reaper.stop();
    CHECK(reaper.reclaimed_total() > 0);
}

static void test_sql_and_simd() {
    std::cout << "Running test_sql_and_simd...\n";
    // Direct SIMD filter check
    std::vector<int64_t> vals(200);
    for (size_t i = 0; i < vals.size(); ++i) vals[i] = static_cast<int64_t>(i);
    auto idx = zephyr::sql::filter_column_i64(vals.data(), vals.size(), 150, zephyr::cpp26::simd::cmp_op::ge);
    CHECK(idx.size() == 50);
    CHECK(idx.front() == 150 && idx.back() == 199);

    // Reflection JSON check
    PlayerStat ps{42, 99.5, true};
    auto j = zephyr::cpp26::reflection::to_json(ps);
    CHECK(j.find("\"id\":42") != std::string::npos);
    CHECK(j.find("\"active\":true") != std::string::npos);

    ZephyrConfig cfg = zephyr_default_config();
    ZephyrDB* db = zephyr_db_open(&cfg);
    CHECK(db != nullptr);

    uint64_t affected = 0;
    CHECK(zephyr_exec(db, "CREATE TABLE players (id BIGINT PRIMARY KEY, name TEXT, gold BIGINT, guild_id BIGINT, hp DOUBLE PRECISION, online BOOLEAN);", &affected) == ZEPHYR_OK);
    CHECK(zephyr_exec(db, "CREATE TABLE guilds (id BIGINT PRIMARY KEY, title TEXT);", &affected) == ZEPHYR_OK);
    CHECK(zephyr_exec(db, "INSERT INTO guilds VALUES (1, 'Vanguard'), (2, 'Shadow');", &affected) == ZEPHYR_OK);
    CHECK(affected == 2);

    // INSERT ... RETURNING
    ZephyrResultSet* ins = zephyr_query(db,
        "INSERT INTO players (id, name, gold, guild_id, hp, online) VALUES "
        "(1, 'Aria', 100, 1, 95.5, TRUE), "
        "(2, 'Bram', 250, 1, 80.0, TRUE), "
        "(3, 'Cora', 400, 2, 60.5, FALSE), "
        "(4, 'Dax',  50,  2, 100.0, TRUE) "
        "RETURNING id, name, gold;");
    CHECK(ins != nullptr && ins->status == ZEPHYR_OK);
    CHECK(ins->row_count == 4 && ins->col_count == 3);
    CHECK((ins->flags & ZEPHYR_FLAG_RETURNING) != 0);
    CHECK(ins->values[0].as.i64 == 1);
    CHECK(std::strcmp(reinterpret_cast<const char*>(ins->values[1].as.slice.ptr), "Aria") == 0);
    zephyr_result_free(ins);

    // O(1) Augmented B-Tree COUNT(*) and SUM(gold)
    ZephyrResultSet* agg = zephyr_query(db, "SELECT COUNT(*), SUM(gold) FROM players;");
    CHECK(agg != nullptr && agg->status == ZEPHYR_OK);
    CHECK((agg->flags & ZEPHYR_FLAG_O1_FASTPATH) != 0);
    CHECK(agg->row_count == 1 && agg->col_count == 2);
    CHECK(agg->values[0].as.i64 == 4);
    CHECK(agg->values[1].as.i64 == 800);
    zephyr_result_free(agg);

    // C-ABI direct O(1) probes
    ZephyrAggregate ca = zephyr_aggregate_count(db, "players");
    CHECK(ca.status == ZEPHYR_OK && ca.count == 4);
    ZephyrAggregate sa = zephyr_aggregate_sum(db, "players", "gold");
    CHECK(sa.status == ZEPHYR_OK && sa.sum_i64 == 800);

    // WITH CTE + JOIN + ORDER BY + LIMIT
    ZephyrResultSet* cte = zephyr_query(db,
        "WITH rich AS (SELECT id, name, gold, guild_id FROM players WHERE gold >= 100) "
        "SELECT r.name, g.title, r.gold FROM rich r INNER JOIN guilds g ON r.guild_id = g.id "
        "ORDER BY r.gold DESC LIMIT 2;");
    CHECK(cte != nullptr && cte->status == ZEPHYR_OK);
    CHECK(cte->row_count == 2 && cte->col_count == 3);
    CHECK(std::strcmp(reinterpret_cast<const char*>(cte->values[0].as.slice.ptr), "Cora") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(cte->values[1].as.slice.ptr), "Shadow") == 0);
    CHECK(cte->values[2].as.i64 == 400);
    zephyr_result_free(cte);

    // GROUP BY
    ZephyrResultSet* grp = zephyr_query(db, "SELECT guild_id, COUNT(*), SUM(gold) FROM players GROUP BY guild_id ORDER BY guild_id ASC;");
    CHECK(grp != nullptr && grp->status == ZEPHYR_OK && grp->row_count == 2);
    CHECK(grp->values[0].as.i64 == 1 && grp->values[1].as.i64 == 2 && grp->values[2].as.i64 == 350);
    CHECK(grp->values[3].as.i64 == 2 && grp->values[4].as.i64 == 2 && grp->values[5].as.i64 == 450);
    zephyr_result_free(grp);

    // Positional $1 parameter query
    ZephyrValue param{};
    param.type = ZEPHYR_TYPE_I64;
    param.as.i64 = 200;
    ZephyrResultSet* pq = zephyr_query_params(db, "SELECT name FROM players WHERE gold > $1 ORDER BY id ASC;", &param, 1);
    CHECK(pq != nullptr && pq->status == ZEPHYR_OK && pq->row_count == 2);
    CHECK(std::strcmp(reinterpret_cast<const char*>(pq->values[0].as.slice.ptr), "Bram") == 0);
    CHECK(std::strcmp(reinterpret_cast<const char*>(pq->values[1].as.slice.ptr), "Cora") == 0);
    zephyr_result_free(pq);

    // UPDATE ... RETURNING & DELETE ... RETURNING
    ZephyrResultSet* upd = zephyr_query(db, "UPDATE players SET gold = gold + 50 WHERE id = 1 RETURNING id, gold;");
    CHECK(upd != nullptr && upd->status == ZEPHYR_OK && upd->row_count == 1);
    CHECK(upd->values[1].as.i64 == 150);
    zephyr_result_free(upd);

    ZephyrResultSet* del = zephyr_query(db, "DELETE FROM players WHERE id = 4 RETURNING name;");
    CHECK(del != nullptr && del->status == ZEPHYR_OK && del->row_count == 1);
    CHECK(std::strcmp(reinterpret_cast<const char*>(del->values[0].as.slice.ptr), "Dax") == 0);
    zephyr_result_free(del);

    // Transactions: BEGIN / ROLLBACK / COMMIT
    CHECK(zephyr_exec(db, "BEGIN;", nullptr) == ZEPHYR_OK);
    CHECK(zephyr_exec(db, "INSERT INTO players VALUES (99, 'Temp', 9999, 1, 10.0, TRUE);", nullptr) == ZEPHYR_OK);
    CHECK(zephyr_aggregate_count(db, "players").count == 4);
    CHECK(zephyr_exec(db, "ROLLBACK;", nullptr) == ZEPHYR_OK);
    CHECK(zephyr_aggregate_count(db, "players").count == 3);

    // JSON export
    ZephyrResultSet* sel = zephyr_query(db, "SELECT id, name FROM players ORDER BY id ASC;");
    uint32_t json_len = 0;
    const char* json = zephyr_result_to_json(db, sel, &json_len);
    CHECK(json != nullptr && json_len > 0);
    CHECK(std::string_view(json, json_len).find("Aria") != std::string_view::npos);
    zephyr_result_free(sel);

    zephyr_db_close(db);
}

static void test_persistence_wal_and_mmap() {
    std::cout << "Running test_persistence_wal_and_mmap...\n";
    fs::path dir = fs::temp_directory_path() / "zephyrdb_test_persist";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string dir_str = dir.string();

    ZephyrConfig cfg = zephyr_default_config();
    cfg.data_dir = dir_str.c_str();
    cfg.wal_flush_ms = 1;
    cfg.enable_wal = 1;
    cfg.enable_compactor = 0;

    {
        ZephyrDB* db = zephyr_db_open(&cfg);
        CHECK(db != nullptr);
        CHECK(zephyr_exec(db, "CREATE TABLE inv (id BIGINT PRIMARY KEY, item TEXT, qty BIGINT);", nullptr) == ZEPHYR_OK);
        CHECK(zephyr_exec(db, "INSERT INTO inv VALUES (1, 'Potion', 5), (2, 'Sword', 1);", nullptr) == ZEPHYR_OK);
        zephyr_db_close(db);
    }

    // Reopen -> WAL replay
    {
        ZephyrDB* db = zephyr_db_open(&cfg);
        CHECK(db != nullptr);
        CHECK(zephyr_aggregate_count(db, "inv").count == 2);
        CHECK(zephyr_aggregate_sum(db, "inv", "qty").sum_i64 == 6);
        CHECK(zephyr_checkpoint(db) == ZEPHYR_OK);
        CHECK(zephyr_exec(db, "INSERT INTO inv VALUES (3, 'Shield', 2);", nullptr) == ZEPHYR_OK);
        zephyr_db_close(db);
    }

    // Reopen -> Mmap snapshot + trailing WAL
    {
        ZephyrDB* db = zephyr_db_open(&cfg);
        CHECK(db != nullptr);
        CHECK(zephyr_aggregate_count(db, "inv").count == 3);
        CHECK(zephyr_aggregate_sum(db, "inv", "qty").sum_i64 == 8);
        zephyr_db_close(db);
    }

    fs::remove_all(dir);
}

static void test_raft_cluster() {
    std::cout << "Running test_raft_cluster...\n";
    zephyr::net::InProcessClusterBus bus;
    std::mutex apply_mu;
    std::vector<std::string> applied[3];

    zephyr::net::RaftNode nodes[3];
    nodes[0].start_in_process(1, bus, {2, 3}, [&](uint64_t, std::string_view sql) {
        std::lock_guard<std::mutex> lk(apply_mu);
        applied[0].emplace_back(sql);
    });
    nodes[1].start_in_process(2, bus, {1, 3}, [&](uint64_t, std::string_view sql) {
        std::lock_guard<std::mutex> lk(apply_mu);
        applied[1].emplace_back(sql);
    });
    nodes[2].start_in_process(3, bus, {1, 2}, [&](uint64_t, std::string_view sql) {
        std::lock_guard<std::mutex> lk(apply_mu);
        applied[2].emplace_back(sql);
    });

    int leader_idx = -1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (int i = 0; i < 3; ++i) {
            if (nodes[i].state() == zephyr::net::RaftState::Leader) {
                leader_idx = i;
                break;
            }
        }
        if (leader_idx >= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(leader_idx >= 0);

    if (leader_idx >= 0) {
        CHECK(nodes[leader_idx].propose_wal_entry(1, "INSERT INTO shards VALUES (1, 'eu-west');"));
        bool all_applied = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            {
                std::lock_guard<std::mutex> lk(apply_mu);
                if (applied[0].size() == 1 && applied[1].size() == 1 && applied[2].size() == 1) {
                    all_applied = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        CHECK(all_applied);
    }

    for (auto& n : nodes) n.stop();
}

static void write_be32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static bool send_all(z_sock_t s, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = send(s, reinterpret_cast<const char*>(data + off), static_cast<int>(len - off), 0);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

static void test_pgwire_loopback() {
    std::cout << "Running test_pgwire_loopback...\n";
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ZephyrConfig cfg = zephyr_default_config();
    ZephyrDB* db = zephyr_db_open(&cfg);
    CHECK(db != nullptr);
    CHECK(zephyr_exec(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, val BIGINT);", nullptr) == ZEPHYR_OK);
    CHECK(zephyr_exec(db, "INSERT INTO t VALUES (1, 100), (2, 250);", nullptr) == ZEPHYR_OK);

    zephyr::net::PgWireServer pg;
    CHECK(pg.start(db, 0));
    uint16_t port = pg.bound_port();
    CHECK(port > 0);

    z_sock_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(sock != Z_INVALID_SOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);

    // Send PGWire v3 StartupMessage (len, 196608, "user\0postgres\0\0")
    std::vector<uint8_t> startup;
    write_be32(startup, 0);
    write_be32(startup, 196608);
    const char kv[] = "user\0postgres\0";
    startup.insert(startup.end(), kv, kv + sizeof(kv)); // includes final \0
    uint32_t slen = static_cast<uint32_t>(startup.size());
    startup[0] = static_cast<uint8_t>((slen >> 24) & 0xFF);
    startup[1] = static_cast<uint8_t>((slen >> 16) & 0xFF);
    startup[2] = static_cast<uint8_t>((slen >> 8) & 0xFF);
    startup[3] = static_cast<uint8_t>(slen & 0xFF);
    CHECK(send_all(sock, startup.data(), startup.size()));

    // Read until ReadyForQuery ('Z')
    std::string recv_buf;
    char tmp[1024];
    for (int iter = 0; iter < 50; ++iter) {
        int r = recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        recv_buf.append(tmp, tmp + r);
        if (recv_buf.find('Z') != std::string::npos) break;
    }
    CHECK(recv_buf.find('R') != std::string::npos);
    CHECK(recv_buf.find('Z') != std::string::npos);

    // Send SimpleQuery ('Q')
    const char* sql = "SELECT COUNT(*), SUM(val) FROM t;";
    std::vector<uint8_t> qmsg;
    qmsg.push_back('Q');
    write_be32(qmsg, static_cast<uint32_t>(4 + std::strlen(sql) + 1));
    qmsg.insert(qmsg.end(), sql, sql + std::strlen(sql) + 1);
    CHECK(send_all(sock, qmsg.data(), qmsg.size()));

    recv_buf.clear();
    for (int iter = 0; iter < 50; ++iter) {
        int r = recv(sock, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        recv_buf.append(tmp, tmp + r);
        if (recv_buf.find('C') != std::string::npos && recv_buf.find('Z') != std::string::npos) break;
    }
    CHECK(recv_buf.find('T') != std::string::npos);
    CHECK(recv_buf.find('D') != std::string::npos);
    CHECK(recv_buf.find("350") != std::string::npos);

    // Terminate ('X')
    uint8_t term[5] = {'X', 0, 0, 0, 4};
    send_all(sock, term, sizeof(term));
    z_close_sock(sock);

    pg.stop();
    zephyr_db_close(db);
#if defined(_WIN32)
    WSACleanup();
#endif
}

static void test_upsert_batch() {
    std::cout << "Running test_upsert_batch...\n";
    zephyr::core::RcuReaper reaper;
    reaper.start(1);
    zephyr::core::AugmentedBTree tree(&reaper);

    std::vector<zephyr::core::AugmentedBTree::BatchEntry> batch;
    batch.reserve(500);
    for (int i = 1; i <= 500; ++i) {
        zephyr::core::AugmentedBTree::BatchEntry e;
        e.key = i;
        e.row_id = static_cast<uint64_t>(i * 2);
        e.i64_vals[0] = i * 10;
        e.f64_vals[0] = static_cast<double>(i);
        batch.push_back(e);
    }
    tree.upsert_batch(batch);
    CHECK(tree.total_count() == 500);
    CHECK(tree.total_sum_i64(0) == 10LL * 500 * 501 / 2);
    uint64_t rid = 0;
    CHECK(tree.find(250, rid) && rid == 500);
    CHECK(tree.range_sum_i64(1, 10, 0) == 550);
    reaper.stop();
}

static void test_prepared_and_concurrent_sql() {
    std::cout << "Running test_prepared_and_concurrent_sql...\n";
    ZephyrConfig cfg = zephyr_default_config();
    ZephyrDB* db = zephyr_db_open(&cfg);
    CHECK(db != nullptr);
    CHECK(zephyr_exec(db, "CREATE TABLE scores (id BIGINT PRIMARY KEY, name TEXT, pts BIGINT);", nullptr) == ZEPHYR_OK);

    ZephyrStmt* ins = zephyr_prepare(db, "INSERT INTO scores VALUES ($1, $2, $3);");
    CHECK(ins != nullptr);
    for (int i = 1; i <= 100; ++i) {
        std::string pname = "P" + std::to_string(i);
        ZephyrValue params[3]{};
        params[0].type = ZEPHYR_TYPE_I64;
        params[0].as.i64 = i;
        params[1].type = ZEPHYR_TYPE_TEXT;
        params[1].as.slice.ptr = reinterpret_cast<const uint8_t*>(pname.data());
        params[1].as.slice.len = static_cast<uint32_t>(pname.size());
        params[2].type = ZEPHYR_TYPE_I64;
        params[2].as.i64 = i * 10;
        uint64_t aff = 0;
        CHECK(zephyr_stmt_exec(ins, params, 3, &aff) == ZEPHYR_OK && aff == 1);
    }
    zephyr_stmt_free(ins);

    // Direct-to-Arena fast projection check + Prepared SELECT
    ZephyrStmt* sel = zephyr_prepare(db, "SELECT id, name, pts FROM scores WHERE pts >= $1;");
    CHECK(sel != nullptr);

    std::atomic<int> err_count{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&]() {
            ZephyrValue p{};
            p.type = ZEPHYR_TYPE_I64;
            p.as.i64 = 900;
            for (int iter = 0; iter < 50; ++iter) {
                ZephyrResultSet* r = zephyr_stmt_query(sel, &p, 1);
                if (!r || r->status != ZEPHYR_OK || r->row_count != 11) {
                    err_count.fetch_add(1, std::memory_order_relaxed);
                }
                zephyr_result_free(r);
            }
        });
    }
    for (auto& th : readers) th.join();
    CHECK(err_count.load() == 0);

    zephyr_stmt_free(sel);
    zephyr_db_close(db);
}

static void test_concurrent_writes_pk_update_and_sys_tables() {
    std::cout << "Running test_concurrent_writes_pk_update_and_sys_tables...\n";
    fs::path dir = fs::temp_directory_path() / "zephyrdb_test_sync_wal";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string dir_str = dir.string();

    ZephyrConfig cfg = zephyr_default_config();
    cfg.data_dir = dir_str.c_str();
    cfg.wal_flush_ms = 2;
    cfg.enable_wal = 1;
    cfg.enable_compactor = 0;

    ZephyrDB* db = zephyr_db_open(&cfg);
    CHECK(db != nullptr);

    for (int t = 0; t < 4; ++t) {
        std::string ddl = "CREATE TABLE t" + std::to_string(t) + " (id BIGINT PRIMARY KEY, val BIGINT);";
        CHECK(zephyr_exec(db, ddl.c_str(), nullptr) == ZEPHYR_OK);
    }

    // 4 concurrent writer threads writing to 4 distinct tables via per-table write_mu
    std::atomic<int> write_errs{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t]() {
            std::string sql = "INSERT INTO t" + std::to_string(t) + " VALUES ($1, $2);";
            ZephyrStmt* st = zephyr_prepare(db, sql.c_str());
            if (!st) {
                write_errs.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 1; i <= 100; ++i) {
                ZephyrValue p[2]{};
                p[0].type = ZEPHYR_TYPE_I64; p[0].as.i64 = i;
                p[1].type = ZEPHYR_TYPE_I64; p[1].as.i64 = i * 10;
                uint64_t aff = 0;
                if (zephyr_stmt_exec(st, p, 2, &aff) != ZEPHYR_OK || aff != 1) {
                    write_errs.fetch_add(1, std::memory_order_relaxed);
                }
            }
            zephyr_stmt_free(st);
        });
    }
    for (auto& w : writers) w.join();
    CHECK(write_errs.load() == 0);

    for (int t = 0; t < 4; ++t) {
        std::string tn = "t" + std::to_string(t);
        CHECK(zephyr_aggregate_count(db, tn.c_str()).count == 100);
    }

    // Precompiled PK point-update
    ZephyrStmt* upd = zephyr_prepare(db, "UPDATE t0 SET val = val + $1 WHERE id = $2;");
    CHECK(upd != nullptr);
    ZephyrValue up_p[2]{};
    up_p[0].type = ZEPHYR_TYPE_I64; up_p[0].as.i64 = 500;
    up_p[1].type = ZEPHYR_TYPE_I64; up_p[1].as.i64 = 1;
    uint64_t uaff = 0;
    CHECK(zephyr_stmt_exec(upd, up_p, 2, &uaff) == ZEPHYR_OK && uaff == 1);
    zephyr_stmt_free(upd);
    CHECK(zephyr_aggregate_sum(db, "t0", "val").sum_i64 == (10LL * 100 * 101 / 2) + 500);

    // Strict OS-flushed sync write
    uint64_t saff = 0;
    CHECK(zephyr_exec_sync(db, "INSERT INTO t0 VALUES (101, 999);", &saff) == ZEPHYR_OK && saff == 1);

    // Virtual system tables
    ZephyrResultSet* vt = zephyr_query(db, "SELECT table_name, row_count FROM information_schema.tables ORDER BY table_name;");
    CHECK(vt != nullptr && vt->status == ZEPHYR_OK && vt->row_count == 4);
    zephyr_result_free(vt);

    ZephyrResultSet* vc = zephyr_query(db, "SELECT column_name, is_primary_key FROM information_schema.columns WHERE table_name = 't0';");
    CHECK(vc != nullptr && vc->status == ZEPHYR_OK && vc->row_count == 2);
    zephyr_result_free(vc);

    ZephyrStats stats = zephyr_get_stats(db);
    CHECK(stats.table_count == 4);
    CHECK(stats.total_rows == 401);
    CHECK(stats.wal_lsn > 0);
    CHECK(stats.wal_flushed_lsn >= stats.wal_lsn);

    zephyr_db_close(db);
    fs::remove_all(dir);
}

static void test_studio_http_loopback() {
    std::cout << "Running test_studio_http_loopback...\n";
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ZephyrConfig cfg = zephyr_default_config();
    ZephyrDB* db = zephyr_db_open(&cfg);
    CHECK(db != nullptr);
    CHECK(zephyr_exec(db, "CREATE TABLE t0 (id BIGINT PRIMARY KEY, val BIGINT);", nullptr) == ZEPHYR_OK);
    CHECK(zephyr_exec(db, "INSERT INTO t0 VALUES (1, 100), (2, 250);", nullptr) == ZEPHYR_OK);

    zephyr::net::StudioServer studio;
    CHECK(studio.start(db, 0));
    uint16_t port = studio.bound_port();
    CHECK(port > 0);

    auto http_req = [&](const std::string& req) -> std::string {
        z_sock_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == Z_INVALID_SOCK) return "";
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            z_close_sock(sock);
            return "";
        }
        send_all(sock, reinterpret_cast<const uint8_t*>(req.data()), req.size());
        std::string out;
        char buf[2048];
        for (;;) {
            int r = recv(sock, buf, sizeof(buf), 0);
            if (r <= 0) break;
            out.append(buf, buf + r);
        }
        z_close_sock(sock);
        return out;
    };

    std::string stats_resp = http_req("GET /api/stats HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(stats_resp.find("200 OK") != std::string::npos);
    CHECK(stats_resp.find("\"table_count\":1") != std::string::npos);

    const std::string body = R"({"sql":"SELECT COUNT(*), SUM(val) FROM t0;"})";
    std::string query_req = "POST /api/query HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    std::string query_resp = http_req(query_req);
    CHECK(query_resp.find("200 OK") != std::string::npos);
    CHECK(query_resp.find("\"o1_fastpath\":true") != std::string::npos);
    CHECK(query_resp.find("350") != std::string::npos);

    studio.stop();
    zephyr_db_close(db);
#if defined(_WIN32)
    WSACleanup();
#endif
}

int main() {
    test_arena();
    test_btree_and_rcu_concurrent();
    test_upsert_batch();
    test_sql_and_simd();
    test_prepared_and_concurrent_sql();
    test_concurrent_writes_pk_update_and_sys_tables();
    test_persistence_wal_and_mmap();
    test_raft_cluster();
    test_pgwire_loopback();
    test_studio_http_loopback();

    if (g_failures == 0) {
        std::cout << "\nALL ZEPHYRDB TESTS PASSED!\n";
        return 0;
    }
    std::cerr << "\nZEPHYRDB TESTS FAILED: " << g_failures << " failure(s).\n";
    return 1;
}
