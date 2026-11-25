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
| **Arena Bump Allocation** (`24 B`, reset/16k) | **1.76 ns/op** | 64-byte aligned slab allocator |
| **Augmented B-Tree Insert** (`100k` rows) | **244.39 ms** (`0.41 M ops/s`) | NodePool recycled CoW writes |
| **Augmented B-Tree `upsert_batch`** (`100k` rows) | **113.65 ms** (`0.88 M ops/s`) | Amortized CoW batch + single RCU root swap |
| **Augmented B-Tree `O(1)` `COUNT` / `SUM`** (`100k` rows) | **6.32 ns/query** | Root metadata read (`ZEPHYR_FLAG_O1_FASTPATH`) |
| **Augmented B-Tree `O(log N)` `range_sum`** (`50k`-key range) | **121.15 ns/query** | Subtree aggregate pruning |
| **SIMD Columnar Filter** (`100k` `int64_t`) | **2,154.38 M rows/sec** | AVX2 4-wide 64-element bitmask chunks |
| **Lock-Free RCU Point Reads** (`4` threads) | **196.39 M reads/sec** | Zero mutexes on read path |
| **SQL C-ABI `COUNT(*) + SUM(gold)` Roundtrip** | **2.62 µs/query** | Full SQL parse + O(1) exec + Arena pack |
| **SQL Prepared `O(1)` `COUNT(*) + SUM(gold)`** (`zephyr_stmt_query`) | **0.81 µs/query** | Cached AST + O(1) exec + Arena pack |
| **SQL Prepared Concurrent Reads** (`4` threads) | **1.66 M queries/sec** | `std::shared_mutex` multi-reader scaling |
