#include "zephyr/c_abi.h"
#include "zephyr/core/arena.hpp"
#include "zephyr/core/augmented_btree.hpp"
#include "zephyr/core/rcu.hpp"
#include "zephyr/sql/simd_eval.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

int main() {
    std::cout << std::unitbuf << "=== ZephyrDB Benchmarks ===\n";

    // 1. Arena bump allocation
    {
        zephyr::core::Arena arena(1024 * 1024);
        constexpr int ITERS = 1000000;
        auto t0 = Clock::now();
        uintptr_t sink = 0;
        for (int i = 0; i < ITERS; ++i) {
            void* p = arena.alloc_bytes(24, 8);
            sink ^= reinterpret_cast<uintptr_t>(p);
            if ((i & 0x3FFF) == 0x3FFF) arena.reset();
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / ITERS;
        std::cout << std::fixed << std::setprecision(2)
                  << "Arena alloc (24B, reset/16k): " << ns << " ns/op (sink=" << (sink & 1) << ")\n";
    }

    // 2. Augmented B-Tree 100k rows: O(1) COUNT/SUM vs O(log N) range
    zephyr::core::RcuReaper reaper;
    reaper.start(5);
    zephyr::core::AugmentedBTree tree(&reaper);
    constexpr int ROWS = 100000;
    {
        auto t0 = Clock::now();
        for (int i = 1; i <= ROWS; ++i) {
            std::array<int64_t, 8> i64s{};
            i64s[0] = i;
            tree.upsert(i, static_cast<uint64_t>(i), i64s);
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "Augmented B-Tree insert 100k: " << ms << " ms (" << (ROWS / (ms / 1000.0) / 1e6) << " M ops/s)\n";
    }

    // 2b. Augmented B-Tree upsert_batch 100k rows
    {
        zephyr::core::AugmentedBTree batch_tree(&reaper);
        std::vector<zephyr::core::AugmentedBTree::BatchEntry> batch(ROWS);
        for (int i = 0; i < ROWS; ++i) {
            batch[i].key = i + 1;
            batch[i].row_id = static_cast<uint64_t>(i + 1);
            batch[i].i64_vals[0] = i + 1;
        }
        auto t0 = Clock::now();
        batch_tree.upsert_batch(batch);
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "Augmented B-Tree upsert_batch 100k: " << ms << " ms (" << (ROWS / (ms / 1000.0) / 1e6) << " M ops/s)\n";
    }

    {
        constexpr int PROBES = 1000000;
        uint64_t sink = 0;
        auto t0 = Clock::now();
        for (int i = 0; i < PROBES; ++i) {
            sink += tree.total_count();
            sink += static_cast<uint64_t>(tree.total_sum_i64(0));
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (PROBES * 2.0);
        std::cout << "Augmented B-Tree O(1) COUNT/SUM: " << ns << " ns/query (sink=" << (sink & 1) << ")\n";
    }

    {
        constexpr int PROBES = 100000;
        int64_t sink = 0;
        auto t0 = Clock::now();
        for (int i = 0; i < PROBES; ++i) {
            sink += tree.range_sum_i64(25000, 75000, 0);
        }
        auto t1 = Clock::now();
        double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / PROBES;
        std::cout << "Augmented B-Tree O(log N) range_sum: " << ns << " ns/query (sink=" << (sink & 1) << ")\n";
    }

    // 3. SIMD vectorized columnar scan
    {
        std::vector<int64_t> col(ROWS);
        for (int i = 0; i < ROWS; ++i) col[i] = i;
        constexpr int RUNS = 1000;
        size_t total_matches = 0;
        auto t0 = Clock::now();
        for (int r = 0; r < RUNS; ++r) {
            auto idx = zephyr::sql::filter_column_i64(col.data(), col.size(), 90000, zephyr::cpp26::simd::cmp_op::ge);
            total_matches += idx.size();
        }
        auto t1 = Clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        double m_rows_sec = (static_cast<double>(ROWS) * RUNS / sec) / 1e6;
        std::cout << "SIMD columnar filter (100k i64): " << m_rows_sec << " M rows/sec (matches=" << total_matches << ")\n";
    }

    // 4. Lock-free multi-threaded RCU read throughput (4 reader threads)
    {
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> total_reads{0};
        std::vector<std::thread> readers;
        auto t0 = Clock::now();
        for (int t = 0; t < 4; ++t) {
            readers.emplace_back([&]() {
                uint64_t local = 0;
                uint64_t r = 0;
                while (!stop.load(std::memory_order_acquire)) {
                    (void)tree.find(50000, r);
                    ++local;
                }
                total_reads.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.store(true, std::memory_order_release);
        for (auto& th : readers) th.join();
        auto t1 = Clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "RCU lock-free point reads (4 threads): "
                  << (total_reads.load() / sec / 1e6) << " M reads/sec\n";
    }

    // 5. SQL O(1) COUNT(*) + SUM(gold) via C-ABI on 50k rows
    {
        ZephyrConfig cfg = zephyr_default_config();
        ZephyrDB* db = zephyr_db_open(&cfg);
        zephyr_exec(db, "CREATE TABLE items (id BIGINT PRIMARY KEY, gold BIGINT);", nullptr);
        zephyr_exec(db, "BEGIN;", nullptr);
        for (int i = 1; i <= 10000; ++i) {
            std::string q = "INSERT INTO items VALUES (" + std::to_string(i) + ", " + std::to_string(i * 5) + ");";
            zephyr_exec(db, q.c_str(), nullptr);
        }
        zephyr_exec(db, "COMMIT;", nullptr);

        constexpr int Q = 10000;
        auto t0 = Clock::now();
        int64_t sink = 0;
        for (int i = 0; i < Q; ++i) {
            ZephyrResultSet* rs = zephyr_query(db, "SELECT COUNT(*), SUM(gold) FROM items;");
            sink += rs->values[0].as.i64;
            zephyr_result_free(rs);
        }
        auto t1 = Clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / Q;
        std::cout << "SQL C-ABI O(1) COUNT+SUM roundtrip: " << us << " us/query (sink=" << (sink & 1) << ")\n";

        // 5b. Prepared statement O(1) COUNT+SUM roundtrip
        ZephyrStmt* stmt = zephyr_prepare(db, "SELECT COUNT(*), SUM(gold) FROM items;");
        auto p0 = Clock::now();
        int64_t psink = 0;
        for (int i = 0; i < Q; ++i) {
            ZephyrResultSet* rs = zephyr_stmt_query(stmt, nullptr, 0);
            psink += rs->values[0].as.i64;
            zephyr_result_free(rs);
        }
        auto p1 = Clock::now();
        double pus = std::chrono::duration<double, std::micro>(p1 - p0).count() / Q;
        std::cout << "SQL Prepared O(1) COUNT+SUM roundtrip: " << pus << " us/query (sink=" << (psink & 1) << ")\n";

        // 5c. Concurrent 4-thread SQL SELECT via shared_mutex
        std::atomic<uint64_t> sql_reads{0};
        std::vector<std::thread> sreaders;
        auto c0 = Clock::now();
        for (int t = 0; t < 4; ++t) {
            sreaders.emplace_back([&]() {
                uint64_t local = 0;
                for (int i = 0; i < 5000; ++i) {
                    ZephyrResultSet* rs = zephyr_stmt_query(stmt, nullptr, 0);
                    if (rs && rs->row_count == 1) ++local;
                    zephyr_result_free(rs);
                }
                sql_reads.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (auto& th : sreaders) th.join();
        auto c1 = Clock::now();
        double csec = std::chrono::duration<double>(c1 - c0).count();
        std::cout << "SQL Prepared concurrent reads (4 threads): "
                  << (sql_reads.load() / csec / 1e6) << " M queries/sec\n";

        // 5d. Precompiled PK Point-Update
        ZephyrStmt* upd = zephyr_prepare(db, "UPDATE items SET gold = gold + $1 WHERE id = $2;");
        auto u0 = Clock::now();
        for (int i = 0; i < Q; ++i) {
            ZephyrValue p[2]{};
            p[0].type = ZEPHYR_TYPE_I64; p[0].as.i64 = 1;
            p[1].type = ZEPHYR_TYPE_I64; p[1].as.i64 = (i % 10000) + 1;
            zephyr_stmt_exec(upd, p, 2, nullptr);
        }
        auto u1 = Clock::now();
        double uus = std::chrono::duration<double, std::micro>(u1 - u0).count() / Q;
        std::cout << "SQL Precompiled PK point-update: " << uus << " us/op (" << (1.0 / uus) << " M updates/sec)\n";
        zephyr_stmt_free(upd);

        // 5e. Mixed MMORPG Read+Write (2 Readers + 2 Writers on separate tables)
        zephyr_exec(db, "CREATE TABLE items2 (id BIGINT PRIMARY KEY, gold BIGINT);", nullptr);
        zephyr_exec(db, "BEGIN;", nullptr);
        for (int i = 1; i <= 1000; ++i) {
            std::string q = "INSERT INTO items2 VALUES (" + std::to_string(i) + ", " + std::to_string(i * 5) + ");";
            zephyr_exec(db, q.c_str(), nullptr);
        }
        zephyr_exec(db, "COMMIT;", nullptr);

        ZephyrStmt* r_stmt = zephyr_prepare(db, "SELECT COUNT(*), SUM(gold) FROM items;");
        ZephyrStmt* w1 = zephyr_prepare(db, "UPDATE items SET gold = gold + $1 WHERE id = $2;");
        ZephyrStmt* w2 = zephyr_prepare(db, "UPDATE items2 SET gold = gold + $1 WHERE id = $2;");
        std::atomic<bool> stop_mix{false};
        std::atomic<uint64_t> mix_ops{0};
        std::vector<std::thread> mix_threads;
        auto m0 = Clock::now();
        for (int t = 0; t < 2; ++t) {
            mix_threads.emplace_back([&]() {
                uint64_t local = 0;
                while (!stop_mix.load(std::memory_order_acquire)) {
                    ZephyrResultSet* rs = zephyr_stmt_query(r_stmt, nullptr, 0);
                    if (rs) ++local;
                    zephyr_result_free(rs);
                }
                mix_ops.fetch_add(local, std::memory_order_relaxed);
            });
        }
        for (int t = 0; t < 2; ++t) {
            ZephyrStmt* ws = (t == 0) ? w1 : w2;
            mix_threads.emplace_back([&, ws]() {
                uint64_t local = 0;
                int idx = 1;
                while (!stop_mix.load(std::memory_order_acquire)) {
                    ZephyrValue p[2]{};
                    p[0].type = ZEPHYR_TYPE_I64; p[0].as.i64 = 1;
                    p[1].type = ZEPHYR_TYPE_I64; p[1].as.i64 = (idx % 1000) + 1;
                    if (zephyr_stmt_exec(ws, p, 2, nullptr) == ZEPHYR_OK) ++local;
                    ++idx;
                }
                mix_ops.fetch_add(local, std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        stop_mix.store(true, std::memory_order_release);
        for (auto& th : mix_threads) th.join();
        auto m1 = Clock::now();
        double msec = std::chrono::duration<double>(m1 - m0).count();
        std::cout << "Mixed MMORPG 2R+2W throughput: " << (mix_ops.load() / msec / 1e6) << " M ops/sec\n";

        zephyr_stmt_free(r_stmt);
        zephyr_stmt_free(w1);
        zephyr_stmt_free(w2);
        zephyr_stmt_free(stmt);
        zephyr_db_close(db);
    }

    reaper.stop();
    return 0;
}
