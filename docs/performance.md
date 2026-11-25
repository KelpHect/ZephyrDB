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
| **Arena Bump Allocation** (`24 B`, reset/16k) | **1.78 ns/op** | 64-byte aligned slab allocator |
| **Augmented B-Tree Insert** (`100k` rows) | **215.63 ms** (`0.46 M ops/s`) | NodePool + O(1) incremental deltas |
| **Augmented B-Tree `upsert_batch`** (`100k` rows) | **11.74 ms** (`8.52 M ops/s`) | O(N) bulk-load + `batch_gen` in-place mutation |
| **Augmented B-Tree `O(1)` `COUNT` / `SUM`** (`100k` rows) | **2.42 ns/query** | Asymmetric RCU + root metadata (`ZEPHYR_FLAG_O1_FASTPATH`) |
| **Augmented B-Tree `O(log N)` `range_sum`** (`50k`-key range) | **108.33 ns/query** | Branchless search + subtree aggregate pruning |
| **SIMD Columnar Filter** (`100k` `int64_t`) | **5,970.33 M rows/sec** | 16-wide unrolled AVX2 + template-hoisted `CmpOp` |
| **Lock-Free RCU Point Reads** (`4` threads) | **139.34 M reads/sec** | Asymmetric RCU (`FlushProcessWriteBuffers`) |
| **SQL C-ABI `COUNT(*) + SUM(gold)` Roundtrip** | **2.09 µs/query** | Full SQL parse + O(1) exec + pooled Arena pack |
| **SQL Prepared `O(1)` `COUNT(*) + SUM(gold)`** (`zephyr_stmt_query`) | **0.05 µs/query** (`50 ns`) | Lock-free `CatalogSnapshot` + precompiled plan + pooled Arena |
| **SQL Prepared Concurrent Reads** (`4` threads) | **34.83 M queries/sec** | 100% lock-free multi-core scaling |
