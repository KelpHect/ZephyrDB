# Tiered Storage, Secondary Indexes & Wire Optimizations

ZephyrDB combines an in-memory **Augmented Copy-on-Write B+Tree** for hot operational state with **immutable memory-mapped cold segments**, **opt-in secondary indexes**, **pipelined PGWire networking**, and **group-commit WAL durability**.

---

## 1. Hot/Cold Tiered Mmap Segments (`TieredStore`)

Defined in `include/zephyr/storage/tiered_store.hpp` and `src/storage/tiered_store.cpp` (`namespace zephyr::storage`), `TieredStore` offloads cold rows out of the hot in-memory table into immutable `.zseg` segment files while preserving `O(1)` aggregate queries and `O(log N)` point/range lookups.

### Data Model & Zone Maps
* **`ColdRow`**: Stores `int64_t key`, `uint64_t row_id`, `std::array<int64_t, 8> i64_cols`, and `std::array<double, 8> f64_cols`.
* **`SegmentZoneMap`**: Each flushed segment tracks `segment_id`, `min_key`, `max_key`, `row_count`, `sum_i64[8]`, `sum_f64[8]`, `encrypted`, and `file_path`.

### Operations
* **`flush_cold_segment(table_name, rows, enc_key32 = nullptr)`**: Sorts `rows` by `key`, computes the `SegmentZoneMap`, and writes an immutable `.zseg` file:
  * **Plaintext (`"ZSEG"`)**: Memory-mapped directly via `MmapStore` for zero-copy reads.
  * **Encrypted (`"ZSNE"`)**: Authenticated and encrypted with **ChaCha20-Poly1305** (`crypto::aead_encrypt`) whenever a 32-byte `enc_key32` is provided.
* **`cold_total_count(table_name)` & `cold_total_sum_i64(table_name, col)`**: Answered in **`O(1)`** over segment count by summing `SegmentZoneMap` metadata without touching disk pages.
* **`cold_range_sum_i64(table_name, lo, hi, col)`**: Prunes non-overlapping segments using `[min_key, max_key]`, adds full-segment `sum_i64` in `O(1)` when `[min_key, max_key] ⊆ [lo, hi]`, and binary-searches only boundary segments.
* **`cold_find(table_name, key, out_row, enc_key32 = nullptr)`**: Prunes segments by `[min_key, max_key]` and binary-searches the matching `MmapStore` (or decrypted buffer).

---

## 2. Opt-In Secondary Indexes & Top-K Joins

### Secondary Indexes (`CREATE [UNIQUE] INDEX` / `DROP INDEX`)
Tables carry `std::vector<SecondaryIndex> indexes` (`i64_map` / `str_map` backed by `std::multimap`). Write paths (`INSERT`, `UPDATE`, `DELETE`) check `!tbl->indexes.empty()`, ensuring **zero overhead** on tables that do not declare secondary indexes.

```sql
CREATE INDEX idx_items_gold ON items (gold);
CREATE UNIQUE INDEX IF NOT EXISTS idx_users_email ON users (email);
DROP INDEX IF EXISTS idx_items_gold;
```

* **`O(log N + K)` `ORDER BY ... LIMIT K [OFFSET O]`**: Single-table queries without `WHERE`/`JOIN`/`GROUP BY`/`DISTINCT` whose `ORDER BY` column has a secondary index stream directly from `idx.i64_map` in forward or reverse order.
* **Equality Point/Multi-Row Lookups**: `WHERE indexed_col = literal|$param` probes `idx.i64_map.equal_range(...)` or `idx.str_map.equal_range(...)` directly instead of scanning all rows.

### Top-K Bounded Sort & Index Nested-Loop Joins
* **Top-K `std::partial_sort`**: Queries with `ORDER BY ... LIMIT K [OFFSET O]` where `O + K < total_rows` run `std::partial_sort` over only the top `O + K` elements instead of a full `std::stable_sort`.
* **Index Nested-Loop Join**: For `INNER JOIN` on `left.col = right.pk` (when `right` has an `int64_t` primary key and `left` has `<= 100,000` rows), the executor probes `right->pk_shard_index[ShardedAugmentedBTree::shard_for_key(pk)]` directly for each outer row instead of materializing a full hash table over `right`.

---

## 3. PGWire `TCP_NODELAY` & `ZephyrPgClient.pipeline()`

### Server-Side `TCP_NODELAY` & Extended Query Pipelining
* `PgWireServer` (`src/net/pgwire.cpp`) sets `TCP_NODELAY` on every accepted TCP client socket (both WinSock2 and POSIX) to eliminate Nagle/delayed-ACK latency spikes on small responses.
* Extended Query pipelining allows clients to send `N` consecutive `Parse ('P')` + `Bind ('B')` + `Execute ('E')` frames followed by a single `Sync ('S')` in one TCP burst, receiving all `N` result sets followed by a single `ReadyForQuery ('Z')`.

### Node.js `ZephyrPgClient.pipeline()` (50 Queries in 1 RTT)
`bindings/node/index.mjs` exposes `async pipeline(queries)` on `ZephyrPgClient`, encoding all queries into one buffer and flushing them in a single `socket.write(...)`:

```js
import { ZephyrPgClient } from './bindings/node/index.mjs';

const client = new ZephyrPgClient({ host: '127.0.0.1', port: 5432 });
await client.connect();

const batch = Array.from({ length: 50 }, (_, i) => ({
  sql: 'SELECT id, gold FROM items WHERE id = $1;',
  params: [i + 1],
}));

// Sends all 50 Parse/Bind/Execute frames + 1 Sync in a single TCP RTT
const results = await client.pipeline(batch);
await client.close();
```

---

## 4. WAL Guardian Group Commit (`set_group_commit_us`)

`WalManager` (`include/zephyr/storage/wal.hpp`, `src/storage/wal.cpp`) batches synchronous ACID commits inside the **WAL Guardian** background thread to amortize OS `FlushFileBuffers` / `fsync` overhead across concurrent writers:

* **`set_group_commit_us(uint32_t us)`**: Configures the coalescing window in microseconds (default: **`250` µs**; set `0` to flush immediately).
* **Coalescing Behavior**: When a synchronous commit (`sync_wait = true`) arrives and fewer than `64` records are queued, the WAL Guardian waits up to `group_commit_us_` (or until `>= 64` records are queued), drains all queued records in **one `fwrite`**, issues **one `FlushFileBuffers` / `fsync`**, advances `flushed_lsn_` and `synced_lsn_`, and wakes all waiting writers together via `flush_cv_.notify_all()`.
* **Observability**: `group_commit_batches()` and `group_commit_records()` report the number of coalesced batches and total records committed through group commit.
