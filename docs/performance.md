# Performance & Internals

ZephyrDB is designed around a simple rule: **never scan rows when tree metadata can answer the query, never block readers for writers, and never allocate per-cell on the FFI boundary.**

---

## 1. Core Architectural Pillars

### Augmented Copy-on-Write (CoW) B+Tree (`O(1)` & `O(log N)`)
`zephyr::core::AugmentedBTree` (order `32`) stores `int64_t key -> row_id` mappings alongside per-node subtree metadata for up to `MAX_AGG_COLS = 8` numeric columns (`count`, `min_key`, `max_key`, `col_sum_i64`, `col_sum_f64`).
* **Global Aggregates (`O(1)`):** `total_count()`, `total_sum_i64(col)`, and `total_sum_f64(col)` read the root node's cached fields directly—independent of table size.
* **Primary-Key Range Aggregates (`O(log N)`):** `range_count(lo, hi)`, `range_sum_i64(lo, hi, col)`, and `range_sum_f64(lo, hi, col)` prune completely-contained interior subtrees in `O(log N)` time instead of scanning leaf rows.
* **Node Pool & Batch Upsert:** `AugmentedBTree::NodePool` recycles up to `4096` freed nodes in a free-list to eliminate allocator churn, while `upsert_batch(entries)` amortizes CoW path copying and RCU root retirement across bulk writes.

### Lock-Free `std::rcu` Reads
Read queries acquire a `zephyr::cpp26::rcu::scoped_read_lock` and traverse an immutable CoW snapshot of the B-Tree root with **zero mutexes**. Writers atomically swap the root pointer and retire replaced nodes to the **RCU Reaper** background agent (`std::execution`), which reclaims memory only after active read grace periods finish.

### Vectorized `std::simd` Columnar Scans
When a query requires a full or partial predicate scan (e.g. non-PK `WHERE col >= val`), `src/sql/simd_eval.cpp` evaluates contiguous columnar arrays in 64-element bitmask chunks using:
* **x86_64:** AVX2 (`_mm256_*`) intrinsics
* **WebAssembly:** WASM `simd128` (`wasm_i64x2_*` / `wasm_f64x2_*`) intrinsics
* **Portable fallback:** Clean scalar loop when SIMD intrinsics are unavailable

### 64-Byte Aligned `Arena` Slab Packing
`zephyr::core::Arena` allocates 64-byte aligned bump-pointer slabs (default `64 KiB`). Every `ZephyrResultSet` is packed into a single `Arena` (`columns[]` → row-major `ZephyrValue[]` → UTF-8/`BYTEA` string tail) and released in one O(1) call via `zephyr_result_free`.

### Prepared Statements (`zephyr_prepare`)
`zephyr_prepare(db, sql)` lexes and parses the SQL statement into a reusable AST once. Subsequent calls to `zephyr_stmt_query` / `zephyr_stmt_exec` bind `$1..$N` parameters directly, skipping lexer and parser overhead on hot paths.

---

## 2. Measured Benchmarks

Measured results from `zephyrdb_bench.exe` on Windows x64 (`MSVC 19.52`, `/std:c++latest /O2 /arch:AVX2`):

| Benchmark | Measured Result | Notes |
| :--- | :--- | :--- |
| **Arena Bump Allocation** (`24 B`, reset/16k) | **1.99 ns/op** | 64-byte aligned slab allocator |
| **Augmented B-Tree Insert** (`100k` rows) | **138.85 ms** (`0.72 M ops/s`) | Per-thread NodePool + TLS RCU retire |
| **Augmented B-Tree `upsert_batch`** (`100k` rows) | **4.48 ms** (`22.33 M ops/s`) | O(N) bulk-load + `batch_gen` in-place mutation |
| **Augmented B-Tree `upsert_batch_columnar`** (`100k` rows) | **0.86 ms** (`115.75 M ops/s`) | **Sub-millisecond** columnar O(N) bulk-load |
| **Augmented B-Tree `O(1)` `COUNT` / `SUM`** (`100k` rows) | **2.09 ns/query** | Single-guard multi-shard asymmetric RCU |
| **Augmented B-Tree `O(log N)` `range_sum`** (`50k`-key range) | **111.55 ns/query** | Branchless search + subtree aggregate pruning |
| **SIMD Columnar Filter** (`100k` `int64_t`) | **6,098.49 M rows/sec** | 16-wide unrolled AVX2 + template-hoisted `CmpOp` |
| **Lock-Free RCU Point Reads** (`4` threads) | **179.51 M reads/sec** | Asymmetric RCU (`FlushProcessWriteBuffers`) |
| **SQL C-ABI `COUNT(*) + SUM(gold)` Roundtrip** | **0.31 µs/query** | Zero-refcount RCU `StmtCacheSnapshot` + O(1) exec |
| **SQL Prepared `O(1)` `COUNT(*) + SUM(gold)`** (`zephyr_stmt_query`) | **0.08 µs/query** (`80 ns`) | Lock-free `CatalogSnapshot` + precompiled plan + pooled Arena |
| **SQL Prepared Concurrent Reads** (`4` threads) | **20.41 M queries/sec** | Sharded `alignas(64)` telemetry counters |
| **SQL Precompiled PK Point-Update** (`UPDATE ... WHERE id = $2`) | **0.33 µs/op** (`3.07 M updates/sec`) | Zero-clone in-place leaf delta + sharded PK index |
| **Same-Table Sharded PK Updates** (`4` threads, 1 hot table) | **8.83 M updates/sec** | 8-way `row_shard_mu` + `pk_shard_index` (no `mu_`/`rw_mu`) |
| **Mixed MMORPG `2R + 2W` Workload** (separate tables) | **27.43 M ops/sec** | Concurrent lock-free reads + parallel table writes |
| **Everyday SQL: 1 `INSERT` / 50 `INSERT`s** (prepared) | **13.50 µs** / **62.70 µs** | ~1.25 µs/row steady-state |
| **Everyday SQL: 1 PK `SELECT` / 50 PK `SELECT`s** (10k rows) | **0.08 µs** / **4.10 µs** | O(1) `CompiledPkSelectPlan` fastpath |
| **Everyday SQL: `JOIN + ORDER BY LIMIT 10`** | **1,868.70 µs** (`1.87 ms`) | Hash join + sort over 10k × 1k rows |
