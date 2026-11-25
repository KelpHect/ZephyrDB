# Node.js Backend Guide

ZephyrDB can power Node.js web backends and game APIs in two ways:
1. **Client-Server over PGWire v3.0 TCP** (`ZephyrPgClient` or `node-postgres` `pg`) connecting to `zephyrdb_server`.
2. **In-Process Embedded N-API Addon** (`bindings/node/`) wrapping `zephyr/c_abi.h` directly in your Node.js process.

---

## 1. Zero-Dependency `ZephyrPgClient` (PGWire v3.0)

`bindings/node/index.mjs` exports `ZephyrPgClient`, a pure `node:net` PostgreSQL Wire Protocol v3.0 client with zero external npm dependencies:
* Calls without `params` use **Simple Query** (`'Q'`).
* Calls with a `params` array use **Extended Query** (`Parse` / `Bind` / `Execute` / `Sync`) with `$1`, `$2`, ... positional parameters.

```js
import { ZephyrPgClient } from "./bindings/node/index.mjs";

const client = new ZephyrPgClient({ host: "127.0.0.1", port: 5432 });
await client.connect();

await client.query(`
  CREATE TABLE IF NOT EXISTS scores (
    id BIGINT PRIMARY KEY,
    player TEXT NOT NULL,
    score BIGINT NOT NULL
  );
`);

// Extended Query with $1, $2, $3 parameters + RETURNING
const inserted = await client.query(
  "INSERT INTO scores (id, player, score) VALUES ($1, $2, $3) RETURNING id, player, score;",
  [1, "Aria", 4200]
);
console.log(inserted.rows[0]);

// O(1) global aggregate
const stats = await client.query("SELECT COUNT(*) AS total_players, SUM(score) AS total_score FROM scores;");
console.log(stats.rows[0]);

await client.close();
```

---

## 2. Using Standard `node-postgres` (`pg`)

Because `zephyrdb_server` speaks PostgreSQL Wire Protocol v3.0, standard `pg` works out of the box:

```js
import pg from "pg";

const pool = new pg.Pool({
  host: "127.0.0.1",
  port: 5432,
  user: "postgres",
  database: "zephyr",
});

const { rows } = await pool.query(
  "SELECT id, player, score FROM scores WHERE score >= $1 ORDER BY score DESC LIMIT 10;",
  [1000]
);
console.log(rows);
await pool.end();
```

---

## 3. Zero-Dependency REST API Example (`http_leaderboard.mjs`)

A complete `node:http` REST API example is included at `bindings/node/examples/http_leaderboard.mjs`. It exposes:
* `GET /leaderboard` — Top players ordered by score
* `GET /stats` — `O(1)` `COUNT(*)` and `SUM(score)`
* `POST /score` — Parameterized `INSERT ... RETURNING`

Start `zephyrdb_server` and run the example:

```powershell
# Terminal 1: Start ZephyrDB PGWire server
.\build\zephyrdb_server.exe --data-dir ./leaderboard_data --pgwire-port 5432

# Terminal 2: Start the Node.js REST API
node bindings/node/examples/http_leaderboard.mjs
```

Run the automated Node.js PGWire test suite:

```powershell
npm --prefix bindings/node test
```

---

## 4. In-Process Embedded Mode (N-API Addon)

For single-process deployments where TCP overhead is unnecessary, `bindings/node/src/zephyr_napi.cpp` provides a native N-API addon wrapping `zephyr_db_open`, `zephyr_db_close`, `zephyr_exec`, `zephyr_query`, `zephyr_prepare`, `zephyr_stmt_query`, `zephyr_stmt_exec`, `zephyr_aggregate_count`, `zephyr_aggregate_sum`, and `zephyr_checkpoint`.

Build the native addon with `node-gyp` inside `bindings/node/` and open it via `openEmbedded(config)`:

```js
import { openEmbedded } from "./bindings/node/index.mjs";

const db = openEmbedded({ dataDir: "./embedded_data" });
db.exec("CREATE TABLE IF NOT EXISTS events (id BIGINT PRIMARY KEY, kind TEXT, value BIGINT);");
db.exec("INSERT INTO events VALUES (1, 'login', 1);");

const res = db.query("SELECT COUNT(*), SUM(value) FROM events;");
console.log(res.rows);

db.close();
```
