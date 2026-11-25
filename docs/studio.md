# Zephyr Studio

**Zephyr Studio** is a zero-dependency embedded HTTP/1.1 Web UI and JSON telemetry server built directly into ZephyrDB (`zephyr::net::StudioServer`). Powered by C++26 `thread_pool_scheduler` and the pure C-ABI, it provides live engine telemetry, a schema explorer, and an interactive SQL editor with `O(1)` Augmented B-Tree fastpath detection.

---

## 1. Launching Zephyr Studio

Pass `--studio-port <port>` when starting `zephyrdb_server` (disabled by default when `0` or omitted):

```powershell
# Windows
.\build\zephyrdb_server.exe --data-dir ./zephyr_data --pgwire-port 5432 --studio-port 8080
```

```bash
# Linux / macOS
./build/zephyrdb_server --data-dir ./zephyr_data --pgwire-port 5432 --studio-port 8080
```

When started, the server prints:

```text
[Zephyr Studio] Web UI at http://127.0.0.1:8080
```

Open `http://127.0.0.1:8080` in any modern browser.

### Embedding `StudioServer` in C++

You can also embed `zephyr::net::StudioServer` directly inside a C++ application or game server alongside an existing `ZephyrDB*` handle:

```cpp
#include "zephyr/c_abi.h"
#include "zephyr/net/studio.hpp"

ZephyrConfig cfg = zephyr_default_config();
cfg.data_dir = "./save_data";
ZephyrDB* db = zephyr_db_open(&cfg);

zephyr::net::StudioServer studio;
studio.start(db, 8080); // Pass 0 to bind an ephemeral OS port (inspect via studio.bound_port())

// ... run your application ...

studio.stop();
zephyr_db_close(db);
```

---

## 2. Web UI Features

* **Live Telemetry Bar**: Polls `GET /api/stats` in real time to display active **Tables**, **Total Rows**, **WAL LSN / Flushed LSN**, **RCU Reclaimed** node count, **O(1) Fastpath Hits**, and **Prepared Queries**, plus a one-click **Checkpoint** button (`POST /api/checkpoint`).
* **Schema Explorer**: Auto-loads tables, row counts, columns, data types, and primary key (`PK`) markers from `GET /api/tables`. Clicking any table runs `SELECT * FROM <table> LIMIT 100;`.
* **SQL Editor**: Monospace editor with `Ctrl+Enter` (`Cmd+Enter` on macOS) execution, nanosecond/microsecond latency badge, an emerald **`O(1) Fastpath`** badge whenever `ZEPHYR_FLAG_O1_FASTPATH` is set, and a **Table / Raw JSON** results toggle.

---

## 3. HTTP JSON API Reference

All endpoints support CORS (`Access-Control-Allow-Origin: *`) and return UTF-8 JSON (except `GET /`).

### `GET /`

Serves the self-contained dark-mode **Zephyr Studio** HTML/CSS/JS application.

### `GET /api/tables`

Queries `information_schema.tables` and `information_schema.columns` via `zephyr_query` and returns the live catalog tree:

```json
{
  "ok": true,
  "tables": [
    {
      "schema": "public",
      "name": "players",
      "type": "BASE TABLE",
      "row_count": 2,
      "columns": [
        {
          "name": "id",
          "ordinal_position": 1,
          "data_type": "BIGINT",
          "is_nullable": "NO",
          "is_primary_key": true
        },
        {
          "name": "gold",
          "ordinal_position": 2,
          "data_type": "BIGINT",
          "is_nullable": "YES",
          "is_primary_key": false
        }
      ]
    }
  ]
}
```

### `POST /api/query`

Executes a SQL statement via `zephyr_query` and measures execution time with `std::chrono::steady_clock`. Accepts either a JSON body `{"sql": "..."}` or a raw SQL request body.

**Request:**

```bash
curl -X POST http://127.0.0.1:8080/api/query \
  -H "Content-Type: application/json" \
  -d '{"sql":"SELECT COUNT(*), SUM(gold) FROM players;"}'
```

**Success Response:**

```json
{
  "ok": true,
  "elapsed_ns": 840,
  "o1_fastpath": true,
  "row_count": 1,
  "rows_affected": 0,
  "columns": ["COUNT(*)", "SUM(gold)"],
  "rows": [[2, 650]]
}
```

**Error Response:**

```json
{
  "ok": false,
  "elapsed_ns": 1200,
  "error": "Table not found: missing_table"
}
```

### `GET /api/stats`

Calls `zephyr_get_stats(db)` and returns live engine telemetry:

```json
{
  "ok": true,
  "table_count": 1,
  "total_rows": 2,
  "wal_lsn": 4,
  "wal_flushed_lsn": 4,
  "rcu_reclaimed": 12,
  "o1_fastpath_hits": 1,
  "prepared_queries": 0,
  "active_txns": 0
}
```

### `POST /api/checkpoint`

Triggers `zephyr_checkpoint(db)` to flush pending WAL frames and compact the memory-mapped snapshot:

```json
{
  "ok": true,
  "status": 0
}
```
