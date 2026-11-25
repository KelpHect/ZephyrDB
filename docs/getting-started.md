# Getting Started

This guide walks through building ZephyrDB from source, embedding the C/C++26 zero-copy C-ABI (including prepared statements), and running the standalone `zephyrdb_server`.

---

## 1. Prerequisites

* **CMake** `3.20+` and **Ninja**
* **C++26-capable compiler:**
  * **Windows:** Visual Studio 2022 / 2026 with MSVC `19.52+` (`/std:c++latest /arch:AVX2`)
  * **Linux / macOS:** GCC `15/16+` or Clang `19+` (`-std=c++26`)

See [C++26 Toolchain](toolchain.md) for the full compiler flag matrix and `include/zephyr/compat/cpp26.hpp` bridge details.

---

## 2. Build Native Core, Shared Library, Tests & Benchmarks

### Windows (MSVC 19.52+ with CMake & Ninja)

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" && cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build"

# Run unit & integration tests
ctest --test-dir build --output-on-failure

# Run benchmarks
.\build\zephyrdb_bench.exe
```

### Linux / macOS (GCC 15/16 or Clang 19+)

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

ctest --test-dir build --output-on-failure
./build/zephyrdb_bench
```

This produces:
* `zephyrdb_core` — Static library (`ZEPHYR_STATIC`)
* `zephyrdb` — Shared C-ABI library (`zephyrdb.dll` / `libzephyrdb.so` / `libzephyrdb.dylib`)
* `zephyrdb_server` — Standalone PGWire v3.0 + Raft Diplomat server
* `zephyrdb_tests` — Unit & integration test suite
* `zephyrdb_bench` — Microbenchmark suite

---

## 3. C / C++26 Quickstart (C-ABI & Prepared Statements)

Link against `zephyrdb` (or `zephyrdb_core` with `-DZEPHYR_STATIC`) and include `zephyr/c_abi.h`.

### Basic Open, Exec, and O(1) Query

```c
#include "zephyr/c_abi.h"
#include <stdio.h>

int main(void) {
    ZephyrConfig cfg = zephyr_default_config();
    cfg.data_dir = "./save_data";

    ZephyrDB* db = zephyr_db_open(&cfg);
    if (!db) return 1;

    zephyr_exec(db, "CREATE TABLE players (id BIGINT PRIMARY KEY, name TEXT, gold BIGINT);", NULL);
    zephyr_exec(db, "INSERT INTO players VALUES (1, 'Aria', 250), (2, 'Bram', 400);", NULL);

    // Resolved in O(1) via Augmented B-Tree root metadata (ZEPHYR_FLAG_O1_FASTPATH)
    ZephyrResultSet* res = zephyr_query(db, "SELECT COUNT(*), SUM(gold) FROM players;");
    if (res && res->status == ZEPHYR_OK) {
        printf("count=%lld sum=%lld fastpath=%u\n",
               (long long)res->values[0].as.i64,
               (long long)res->values[1].as.i64,
               (res->flags & ZEPHYR_FLAG_O1_FASTPATH) ? 1u : 0u);
    }
    zephyr_result_free(res);
    zephyr_db_close(db);
    return 0;
}
```

### Prepared Statements (`zephyr_prepare` / `zephyr_stmt_query`)

For hot loops (e.g. per-frame NPC queries or high-QPS API endpoints), compile the SQL AST once with `zephyr_prepare` and bind `$1..$N` parameters with `zephyr_stmt_query` or `zephyr_stmt_exec`:

```c
#include "zephyr/c_abi.h"

void query_player_by_id(ZephyrDB* db, int64_t player_id) {
    ZephyrStmt* stmt = zephyr_prepare(db, "SELECT id, name, gold FROM players WHERE id = $1;");

    ZephyrValue param = {0};
    param.type = ZEPHYR_TYPE_I64;
    param.as.i64 = player_id;

    ZephyrResultSet* res = zephyr_stmt_query(stmt, &param, 1);
    if (res && res->status == ZEPHYR_OK && res->row_count > 0) {
        // Cell 1 is TEXT: read directly from Arena slice with zero copy
        ZephyrSlice name = res->values[1].as.slice;
        int64_t gold = res->values[2].as.i64;
        (void)name;
        (void)gold;
    }

    zephyr_result_free(res);
    zephyr_stmt_free(stmt);
}
```

---

## 4. Running `zephyrdb_server`

`zephyrdb_server` exposes a PostgreSQL Wire Protocol v3.0 TCP listener and an optional Raft Diplomat UDP replication agent:

```powershell
# Standalone server on port 5432
.\build\zephyrdb_server.exe --data-dir ./zephyr_data --pgwire-port 5432
```

For a replicated MMORPG shard cluster:

```powershell
.\build\zephyrdb_server.exe `
  --data-dir ./shard_1 `
  --pgwire-port 5432 `
  --raft-id 1 `
  --raft-port 7001 `
  --peers "1@127.0.0.1:7001,2@127.0.0.1:7002,3@127.0.0.1:7003"
```

Connect with `psql` or any PostgreSQL driver:

```bash
psql -h 127.0.0.1 -p 5432 -U postgres
```
