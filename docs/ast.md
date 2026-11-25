# ZephyrDB Postgres Syntax Subset & AST Limits

ZephyrDB implements a compact, zero-allocation-friendly recursive-descent parser for a strict subset of PostgreSQL SQL. Keeping the grammar bounded ensures predictable sub-microsecond parse times and direct mapping to `std::simd` scans and `O(1)` Augmented B-Tree probes.

---

## 1. Supported Postgres Data Types

| SQL Type Aliases | Canonical `ZephyrType` | Storage / Width |
| :--- | :--- | :--- |
| `BIGINT`, `INT`, `INTEGER` | `ZEPHYR_TYPE_I64` | 64-bit signed integer (`int64_t`) |
| `DOUBLE PRECISION`, `FLOAT8` | `ZEPHYR_TYPE_F64` | 64-bit IEEE-754 float (`double`) |
| `BOOLEAN`, `BOOL` | `ZEPHYR_TYPE_BOOL` | 1-byte boolean (`uint8_t`) |
| `TEXT`, `VARCHAR` | `ZEPHYR_TYPE_TEXT` | UTF-8 byte slice (`ZephyrSlice`) |
| `BYTEA` | `ZEPHYR_TYPE_BYTEA` | Raw binary byte slice (`ZephyrSlice`) |

---

## 2. Bounded Keyword Table

All keywords are matched case-insensitively by the `std::string_view` lexer:

| Category | Keywords |
| :--- | :--- |
| **DDL** | `CREATE`, `DROP`, `TABLE`, `IF`, `NOT`, `EXISTS`, `PRIMARY`, `KEY` |
| **DML** | `INSERT`, `INTO`, `VALUES`, `RETURNING`, `UPDATE`, `SET`, `DELETE`, `FROM` |
| **DQL / CTE / Joins** | `WITH`, `AS`, `SELECT`, `DISTINCT`, `JOIN`, `INNER`, `LEFT`, `ON`, `WHERE`, `GROUP`, `BY`, `ORDER`, `ASC`, `DESC`, `LIMIT`, `OFFSET` |
| **Transactions** | `BEGIN`, `TRANSACTION`, `COMMIT`, `ROLLBACK` |
| **Logic & Operators** | `AND`, `OR`, `IS`, `NULL`, `TRUE`, `FALSE` |
| **Types** | `BIGINT`, `INT`, `INTEGER`, `DOUBLE`, `PRECISION`, `FLOAT8`, `BOOLEAN`, `BOOL`, `TEXT`, `VARCHAR`, `BYTEA` |
| **Aggregates** | `COUNT`, `SUM`, `MIN`, `MAX`, `AVG` |

### Lexical Tokens
* **Identifiers:** Unquoted (`[A-Za-z_][A-Za-z0-9_]*`) or double-quoted (`"..."`).
* **Literals:** Single-quoted strings `'...'` (with `''` escape), 64-bit integers, floating-point numbers, `TRUE`/`FALSE`, `NULL`.
* **Parameters:** Positional `$1`, `$2`, ..., `$N` (1-based index into `params` array).
* **Operators & Punctuation:** `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`, `+`, `-`, `*`, `/`, `(`, `)`, `,`, `;`, `.`.

---

## 3. Complete EBNF Grammar

```ebnf
script          ::= stmt { ";" stmt } [ ";" ] ;

stmt            ::= create_table
                  | drop_table
                  | insert_stmt
                  | update_stmt
                  | delete_stmt
                  | query_stmt
                  | tx_stmt ;

create_table    ::= "CREATE" "TABLE" [ "IF" "NOT" "EXISTS" ] ident
                    "(" col_def { "," col_def } ")" ;
col_def         ::= ident data_type { "PRIMARY" "KEY" | "NOT" "NULL" } ;
data_type       ::= "BIGINT" | "INT" | "INTEGER"
                  | "DOUBLE" "PRECISION" | "FLOAT8"
                  | "BOOLEAN" | "BOOL"
                  | "TEXT" | "VARCHAR"
                  | "BYTEA" ;

drop_table      ::= "DROP" "TABLE" [ "IF" "EXISTS" ] ident ;

insert_stmt     ::= "INSERT" "INTO" ident [ "(" ident { "," ident } ")" ]
                    "VALUES" value_tuple { "," value_tuple }
                    [ "RETURNING" select_list ] ;
value_tuple     ::= "(" expr { "," expr } ")" ;

update_stmt     ::= "UPDATE" ident "SET" assign { "," assign }
                    [ "WHERE" expr ]
                    [ "RETURNING" select_list ] ;
assign          ::= ident "=" expr ;

delete_stmt     ::= "DELETE" "FROM" ident
                    [ "WHERE" expr ]
                    [ "RETURNING" select_list ] ;

query_stmt      ::= [ with_clause ] select_core ;
with_clause     ::= "WITH" cte_def { "," cte_def } ;
cte_def         ::= ident "AS" "(" select_core ")" ;

select_core     ::= "SELECT" [ "DISTINCT" ] select_list
                    [ "FROM" table_ref { join_clause } ]
                    [ "WHERE" expr ]
                    [ "GROUP" "BY" expr { "," expr } ]
                    [ "ORDER" "BY" order_item { "," order_item } ]
                    [ "LIMIT" int_lit ]
                    [ "OFFSET" int_lit ] ;

table_ref       ::= ident [ [ "AS" ] ident ] ;
join_clause     ::= [ "INNER" | "LEFT" ] "JOIN" table_ref "ON" expr ;
order_item      ::= expr [ "ASC" | "DESC" ] ;

select_list     ::= select_item { "," select_item } ;
select_item     ::= "*"
                  | expr [ "AS" ident ] ;

tx_stmt         ::= "BEGIN" [ "TRANSACTION" ]
                  | "COMMIT"
                  | "ROLLBACK" ;

expr            ::= or_expr ;
or_expr         ::= and_expr { "OR" and_expr } ;
and_expr        ::= cmp_expr { "AND" cmp_expr } ;
cmp_expr        ::= add_expr [ cmp_op add_expr | "IS" [ "NOT" ] "NULL" ] ;
cmp_op          ::= "=" | "!=" | "<>" | "<" | "<=" | ">" | ">=" ;
add_expr        ::= mul_expr { ( "+" | "-" ) mul_expr } ;
mul_expr        ::= unary_expr { ( "*" | "/" ) unary_expr } ;
unary_expr      ::= ( "-" | "NOT" ) unary_expr | primary ;
primary         ::= literal
                  | "$" int_lit
                  | agg_call
                  | ident [ "." ident ]
                  | "(" expr ")" ;

agg_call        ::= ( "COUNT" | "SUM" | "MIN" | "MAX" | "AVG" )
                    "(" ( "*" | expr ) ")" ;
```

---

## 4. Augmented B-Tree Aggregate Pushdown Rules

The execution engine routes aggregate queries along three tiers based on the AST shape:

| Query Shape | Complexity | Execution Path | `ZephyrResultSet.flags` |
| :--- | :---: | :--- | :---: |
| `SELECT COUNT(*) FROM tbl` or `SELECT SUM(col) FROM tbl` (no `WHERE`, no `GROUP BY`, no `JOIN`) | **`O(1)`** | Direct probe of the Augmented B-Tree root node's cached aggregate metadata (`zephyr_aggregate_count` / `zephyr_aggregate_sum`). | `ZEPHYR_FLAG_O1_FASTPATH` |
| `SELECT COUNT(*)` / `SUM(col)` with primary-key range predicate (`WHERE pk >= a AND pk <= b` or `pk = k`) | **`O(log N)`** | Augmented B-Tree subtree/range boundary traversal summing internal node aggregates without visiting leaf rows. | `0` |
| General `WHERE` predicates, `GROUP BY`, `JOIN`, `MIN`/`MAX`/`AVG`, or non-PK filters | **`O(N / W)`** | Vectorized `std::simd` columnar filter (`filter_chunk_i64` / `filter_chunk_f64` in 64-element batches) + hash aggregation. | `0` |

---

## 5. Explicit Out-of-Scope Postgres Features

To keep the parser compact, deterministic, and embeddable in game engines, the following PostgreSQL features are intentionally **out of scope**:

* **Programmability:** Triggers, stored procedures, `CREATE FUNCTION`, PL/pgSQL blocks, `DO` blocks.
* **Advanced Constraints & DDL:** `FOREIGN KEY`, `CHECK`, `UNIQUE` (non-PK), `ALTER TABLE`, `CREATE INDEX`, `VIEW`s, schemas/namespaces (`schema.table`).
* **Complex Queries:** Window functions (`OVER (...)`, `PARTITION BY`), `HAVING`, set operations (`UNION`, `INTERSECT`, `EXCEPT`), correlated subqueries in `WHERE`/`SELECT`, `LATERAL` joins, `RIGHT`/`FULL OUTER`/`CROSS` joins, `RECURSIVE` CTEs.
* **Misc SQL:** `LIKE`/`ILIKE`/regex operators, `IN (...)` lists, `BETWEEN` (expressible as `>= AND <=`), `CASE WHEN`, `COALESCE`, `CAST`/`::`, `ON CONFLICT` upserts, `SAVEPOINT`.
