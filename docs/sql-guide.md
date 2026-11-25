# SQL Guide

ZephyrDB implements a fast, bounded subset of **PostgreSQL** syntax designed for predictable sub-microsecond parsing, `std::simd` columnar evaluation, and `O(1)` / `O(log N)` Augmented B-Tree pushdowns.

See [Postgres AST & Grammar](ast.md) for the complete EBNF grammar and out-of-scope features list.

---

## 1. Schema Definition (`CREATE TABLE` / `DROP TABLE`)

Supported types map directly to ZephyrDB's zero-copy C-ABI `ZephyrType` enum:
* `BIGINT`, `INT`, `INTEGER` → `ZEPHYR_TYPE_I64` (`int64_t`)
* `DOUBLE PRECISION`, `FLOAT8` → `ZEPHYR_TYPE_F64` (`double`)
* `BOOLEAN`, `BOOL` → `ZEPHYR_TYPE_BOOL` (`uint8_t`)
* `TEXT`, `VARCHAR` → `ZEPHYR_TYPE_TEXT` (UTF-8 `ZephyrSlice`)
* `BYTEA` → `ZEPHYR_TYPE_BYTEA` (raw binary `ZephyrSlice`)

```sql
CREATE TABLE IF NOT EXISTS players (
    id BIGINT PRIMARY KEY,
    name TEXT NOT NULL,
    guild_id BIGINT,
    gold BIGINT,
    rating DOUBLE PRECISION,
    is_online BOOLEAN
);

CREATE TABLE IF NOT EXISTS guilds (
    id BIGINT PRIMARY KEY,
    name TEXT NOT NULL
);
```

```sql
DROP TABLE IF EXISTS old_events;
```

---

## 2. Mutations & `INSERT ... RETURNING`

Multi-row inserts, updates, deletes, and PostgreSQL `RETURNING` clauses are supported:

```sql
INSERT INTO guilds (id, name) VALUES
    (10, 'Dawnblade'),
    (20, 'Ironclad');

INSERT INTO players (id, name, guild_id, gold, rating, is_online)
VALUES (1, 'Aria', 10, 500, 1850.5, TRUE)
RETURNING id, name, gold;
```

```sql
UPDATE players
SET gold = gold + 100
WHERE id = 1
RETURNING id, gold;

DELETE FROM players
WHERE is_online = FALSE
RETURNING id;
```

---

## 3. Positional Parameters (`$1`, `$2`, ...)

Use PostgreSQL-style `$1`, `$2`, ... placeholders with `zephyr_query_params`, `zephyr_stmt_query` / `zephyr_stmt_exec`, or PGWire Extended Query (`ZephyrPgClient` / `node-postgres`):

```sql
INSERT INTO players (id, name, guild_id, gold, rating, is_online)
VALUES ($1, $2, $3, $4, $5, $6)
RETURNING id, name, gold;

SELECT id, name, gold
FROM players
WHERE guild_id = $1 AND gold >= $2
ORDER BY gold DESC
LIMIT $3;
```

---

## 4. `WITH` CTEs, `JOIN`, and `GROUP BY`

Common Table Expressions (`WITH`), `INNER JOIN` / `LEFT JOIN`, `DISTINCT`, `GROUP BY`, `ORDER BY`, `LIMIT`, and `OFFSET` are supported:

```sql
WITH rich_players AS (
    SELECT id, name, guild_id, gold
    FROM players
    WHERE gold >= 250
)
SELECT g.name AS guild_name, COUNT(*) AS members, SUM(rp.gold) AS total_gold
FROM rich_players rp
INNER JOIN guilds g ON rp.guild_id = g.id
GROUP BY g.name
ORDER BY total_gold DESC
LIMIT 10 OFFSET 0;
```

---

## 5. Transactions (`BEGIN` / `COMMIT` / `ROLLBACK`)

Batch multiple mutations inside an explicit transaction to amortize WAL ring-buffer commits and CoW B-Tree root swaps:

```sql
BEGIN;
INSERT INTO players (id, name, guild_id, gold, rating, is_online) VALUES (2, 'Bram', 10, 320, 1420.0, TRUE);
INSERT INTO players (id, name, guild_id, gold, rating, is_online) VALUES (3, 'Cora', 20, 910, 2010.2, FALSE);
COMMIT;
```

If an error occurs or you need to discard uncommitted changes:

```sql
BEGIN;
DELETE FROM players WHERE id = 2;
ROLLBACK;
```

---

## 6. `O(1)` and `O(log N)` Augmented B-Tree Fast Paths

ZephyrDB's Augmented B-Tree caches `COUNT`, `SUM`, `MIN`, and `MAX` metadata on every internal and leaf node.

### `O(1)` Global Aggregates

When a `SELECT` has no `WHERE` or `GROUP BY` clause and requests `COUNT(*)` or `SUM(numeric_col)` (for any of the first 8 numeric columns), ZephyrDB reads the root node's cached aggregate in **O(1)** time and sets `ZEPHYR_FLAG_O1_FASTPATH` on `ZephyrResultSet.flags`:

```sql
-- Resolved in O(1) directly from the Augmented B-Tree root
SELECT COUNT(*), SUM(gold) FROM players;
```

### `O(log N)` Primary-Key Range Aggregates

When filtering on the `PRIMARY KEY` with `BETWEEN` (or `id >= lo AND id <= hi`), ZephyrDB prunes entire subtrees and sums interior node metadata in **O(log N)** time:

```sql
-- Resolved in O(log N) via Augmented B-Tree subtree pruning
SELECT COUNT(*), SUM(gold)
FROM players
WHERE id BETWEEN 1000 AND 50000;
```
