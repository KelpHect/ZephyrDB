# ZephyrDB Zero-Copy C-ABI & Struct Layouts (`include/zephyr/c_abi.h`)

This document specifies the binary interface, exact memory layouts, arena packing, lifetime rules, and zero-copy FFI patterns for **ZephyrDB** (`include/zephyr/c_abi.h`).

---

## 1. Export Macros, Opaque Handles & Enums

### Export Visibility (`ZEPHYR_API`)

| Platform / Configuration | Macro Definition |
| :--- | :--- |
| Windows (`_WIN32` / `__CYGWIN__`), `ZEPHYR_BUILD_DLL` defined | `__declspec(dllexport)` |
| Windows (`_WIN32` / `__CYGWIN__`), `ZEPHYR_STATIC` defined | *(empty)* |
| Windows (`_WIN32` / `__CYGWIN__`), consumer default | `__declspec(dllimport)` |
| GCC / Clang / Emscripten (`__GNUC__` / `__clang__` / `__EMSCRIPTEN__`) | `__attribute__((visibility("default")))` |
| Other | *(empty)* |

### Opaque Handle

```c
typedef struct ZephyrDB ZephyrDB;
```

### Status Codes (`ZephyrStatus`)

| Enumerator | Value (`int32_t`) | Meaning |
| :--- | :---: | :--- |
| `ZEPHYR_OK` | `0` | Success |
| `ZEPHYR_ERR_INVALID_ARG` | `1` | Null pointer, invalid config, or out-of-range parameter |
| `ZEPHYR_ERR_SYNTAX` | `2` | SQL lexer or parser syntax error |
| `ZEPHYR_ERR_NOT_FOUND` | `3` | Table, column, or CTE not found |
| `ZEPHYR_ERR_CONSTRAINT` | `4` | Primary key or `NOT NULL` constraint violation |
| `ZEPHYR_ERR_IO` | `5` | WAL, mmap, or network socket I/O error |
| `ZEPHYR_ERR_INTERNAL` | `6` | Unexpected internal engine error |
| `ZEPHYR_ERR_BUSY` | `7` | Contention / busy state |

### Column & Value Types (`ZephyrType`)

| Enumerator | Value (`int32_t`) | Backing Representation |
| :--- | :---: | :--- |
| `ZEPHYR_TYPE_NULL` | `0` | SQL `NULL` (no payload read) |
| `ZEPHYR_TYPE_I64` | `1` | `ZephyrValue.as.i64` (`int64_t`) |
| `ZEPHYR_TYPE_F64` | `2` | `ZephyrValue.as.f64` (`double`) |
| `ZEPHYR_TYPE_BOOL` | `3` | `ZephyrValue.as.boolean` (`uint8_t`, `0` or `1`) |
| `ZEPHYR_TYPE_TEXT` | `4` | `ZephyrValue.as.slice` (UTF-8 bytes, null-terminated in arena) |
| `ZEPHYR_TYPE_BYTEA` | `5` | `ZephyrValue.as.slice` (raw binary bytes) |

### Result Flags (`ZephyrResultFlags`)

| Flag | Bitmask (`uint32_t`) | Meaning |
| :--- | :---: | :--- |
| `ZEPHYR_FLAG_O1_FASTPATH` | `1u << 0` (`0x1`) | Query was satisfied in `O(1)` via Augmented B-Tree metadata |
| `ZEPHYR_FLAG_RETURNING` | `1u << 1` (`0x2`) | Result set contains rows from an `INSERT`/`UPDATE`/`DELETE ... RETURNING` |

---

## 2. Exact `extern "C"` Function Signatures

All functions use C linkage (`extern "C"`):

```c
ZEPHYR_API ZephyrConfig zephyr_default_config(void);
ZEPHYR_API ZephyrDB* zephyr_db_open(const ZephyrConfig* config);
ZEPHYR_API void zephyr_db_close(ZephyrDB* db);

ZEPHYR_API int32_t zephyr_exec(ZephyrDB* db, const char* sql, uint64_t* rows_affected);
ZEPHYR_API ZephyrResultSet* zephyr_query(ZephyrDB* db, const char* sql);
ZEPHYR_API ZephyrResultSet* zephyr_query_params(
    ZephyrDB* db,
    const char* sql,
    const ZephyrValue* params,
    uint32_t param_count
);
ZEPHYR_API void zephyr_result_free(ZephyrResultSet* result);

/* O(1) Augmented B-Tree direct probes */
ZEPHYR_API ZephyrAggregate zephyr_aggregate_count(ZephyrDB* db, const char* table_name);
ZEPHYR_API ZephyrAggregate zephyr_aggregate_sum(ZephyrDB* db, const char* table_name, const char* col_name);

ZEPHYR_API int32_t zephyr_checkpoint(ZephyrDB* db);
ZEPHYR_API const char* zephyr_last_error(ZephyrDB* db);
ZEPHYR_API const char* zephyr_result_to_json(ZephyrDB* db, const ZephyrResultSet* result, uint32_t* out_len);

/* MMORPG Distributed & Wire Protocol APIs */
ZEPHYR_API int32_t zephyr_raft_start(ZephyrDB* db, uint32_t node_id, uint16_t port, const char* peers_csv);
ZEPHYR_API void zephyr_raft_stop(ZephyrDB* db);
ZEPHYR_API int32_t zephyr_pgwire_start(ZephyrDB* db, uint16_t port);
ZEPHYR_API void zephyr_pgwire_stop(ZephyrDB* db);
```

---

## 3. Struct Memory Layouts (`x64` LP64/LLP64 vs `wasm32`)

All structs are standard-layout C structs with explicit padding fields so compilers do not insert unexpected holes on 64-bit targets.

### 3.1 `ZephyrSlice`
* **`x64` (LP64/LLP64):** `sizeof == 16`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 12`, `alignof == 4`

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `ptr` | `const uint8_t*` | `0` | `8` | `0` | `4` | Pointer to UTF-8 or binary bytes |
| `len` | `uint32_t` | `8` | `4` | `4` | `4` | Byte length (excluding any trailing `\0`) |
| `reserved` | `uint32_t` | `12` | `4` | `8` | `4` | Reserved; must be `0` |

### 3.2 `ZephyrValue`
* **`x64` (LP64/LLP64):** `sizeof == 24`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 24`, `alignof == 8` *(because `int64_t` and `double` are 8-byte aligned in the wasm32 C ABI, forcing the `as` union size to `16` bytes)*

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `type` | `int32_t` | `0` | `4` | `0` | `4` | `ZephyrType` discriminant |
| `_pad` | `uint32_t` | `4` | `4` | `4` | `4` | Explicit alignment padding |
| `as` | `union` | `8` | `16` | `8` | `16` | Active member selected by `type` |
| ↳ `as.i64` | `int64_t` | `8` | `8` | `8` | `8` | Active when `ZEPHYR_TYPE_I64` |
| ↳ `as.f64` | `double` | `8` | `8` | `8` | `8` | Active when `ZEPHYR_TYPE_F64` |
| ↳ `as.boolean` | `uint8_t` | `8` | `1` | `8` | `1` | Active when `ZEPHYR_TYPE_BOOL` |
| ↳ `as.slice` | `ZephyrSlice` | `8` | `16` | `8` | `12` (+4 tail pad) | Active when `TEXT` or `BYTEA` |

### 3.3 `ZephyrColumnDesc`
* **`x64` (LP64/LLP64):** `sizeof == 16`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 12`, `alignof == 4`

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `name` | `const char*` | `0` | `8` | `0` | `4` | UTF-8 column name |
| `name_len` | `uint32_t` | `8` | `4` | `4` | `4` | Byte length of `name` |
| `type` | `int32_t` | `12` | `4` | `8` | `4` | `ZephyrType` of the column |

### 3.4 `ZephyrResultSet`
* **`x64` (LP64/LLP64):** `sizeof == 56`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 48`, `alignof == 8` *(44 bytes of fields + 4 bytes implicit tail padding for 8-byte struct alignment due to `uint64_t`)*

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `row_count` | `uint64_t` | `0` | `8` | `0` | `8` | Number of rows in `values` |
| `col_count` | `uint64_t` | `8` | `8` | `8` | `8` | Number of columns in `columns` |
| `rows_affected` | `uint64_t` | `16` | `8` | `16` | `8` | Affected rows for DML |
| `columns` | `const ZephyrColumnDesc*` | `24` | `8` | `24` | `4` | Array of `col_count` descriptors |
| `values` | `const ZephyrValue*` | `32` | `8` | `28` | `4` | Row-major array (`row_count * col_count`) |
| `_arena_handle` | `void*` | `40` | `8` | `32` | `4` | Opaque owning `Arena*`; do not dereference |
| `status` | `uint32_t` | `48` | `4` | `36` | `4` | `ZephyrStatus` code |
| `flags` | `uint32_t` | `52` | `4` | `40` | `4` | Bitmask of `ZephyrResultFlags` |
| *(tail pad)* | — | — | — | `44` | `4` | Implicit wasm32 alignment padding |

### 3.5 `ZephyrAggregate`
* **`x64` (LP64/LLP64):** `sizeof == 32`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 32`, `alignof == 8`

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `count` | `int64_t` | `0` | `8` | `0` | `8` | Row count (`O(1)` root probe) |
| `sum_i64` | `int64_t` | `8` | `8` | `8` | `8` | Integer sum |
| `sum_f64` | `double` | `16` | `8` | `16` | `8` | Floating-point sum |
| `status` | `int32_t` | `24` | `4` | `24` | `4` | `ZephyrStatus` code |
| `_pad` | `uint32_t` | `28` | `4` | `28` | `4` | Explicit tail padding |

### 3.6 `ZephyrConfig`
* **`x64` (LP64/LLP64):** `sizeof == 32`, `alignof == 8`
* **`wasm32` (ILP32):** `sizeof == 32`, `alignof == 8` *(4 bytes implicit padding at offset `4` before `uint64_t arena_slab_bytes`)*

| Field | C Type | `x64` Offset | `x64` Size | `wasm32` Offset | `wasm32` Size | Notes |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| `data_dir` | `const char*` | `0` | `8` | `0` | `4` | Directory path (or `NULL`/empty for in-memory) |
| *(implicit pad)* | — | — | — | `4` | `4` | Aligns `arena_slab_bytes` to 8 bytes on wasm32 |
| `arena_slab_bytes` | `uint64_t` | `8` | `8` | `8` | `8` | Arena slab size (default `65536` = 64 KiB) |
| `wal_flush_ms` | `uint32_t` | `16` | `4` | `16` | `4` | Background WAL flush interval in ms |
| `enable_wal` | `uint32_t` | `20` | `4` | `20` | `4` | Non-zero to enable WAL persistence |
| `enable_compactor` | `uint32_t` | `24` | `4` | `24` | `4` | Non-zero to enable background Compactor |
| `_pad` | `uint32_t` | `28` | `4` | `28` | `4` | Explicit tail padding |

---

## 4. Arena Result Packing & Lifetime Rules

### Slab Layout

Every query result (`zephyr_query`, `zephyr_query_params`) allocates a dedicated `zephyr::core::Arena` backed by 64-byte aligned chained slabs (default 64 KiB, configurable via `ZephyrConfig.arena_slab_bytes`). All result metadata and variable-length payloads are bump-allocated contiguously inside the arena:

```text
+-----------------------------------------------------------------------+
| 64-byte Aligned Arena Slab(s)                                         |
+-----------------------------------------------------------------------+
| [0] ZephyrResultSet header (56B x64 / 48B wasm32)                    |
|     └─ _arena_handle ──▶ owning Arena*                                |
+-----------------------------------------------------------------------+
| [1] ZephyrColumnDesc[col_count]                                       |
|     └─ name ──────────┐                                               |
+-----------------------|-----------------------------------------------+
| [2] ZephyrValue[row_count * col_count]  (row-major: row * col_count + c)
|     └─ as.slice.ptr ──┼──┐                                            |
+-----------------------|--|--------------------------------------------+
| [3] String & BYTEA Tail  ▼                                            |
|     col_0_name\0 | col_1_name\0 | row0_text\0 | row1_bytea ...        |
+-----------------------------------------------------------------------+
```

* **Zero intermediate copies:** Column names (`ZephyrColumnDesc.name`) and `TEXT`/`BYTEA` slices (`ZephyrValue.as.slice.ptr`) point directly into the arena's string/bytea tail.
* **Row-major indexing:** Cell `(r, c)` is located at `result->values[r * result->col_count + c]`.

### Lifetime Rules

1. **`ZephyrResultSet*`:** Owned by the caller. Valid until passed to `zephyr_result_free(result)`. Calling `zephyr_result_free(NULL)` is a safe no-op.
2. **Embedded pointers (`columns`, `values`, `ZephyrSlice.ptr`, `ZephyrColumnDesc.name`):** Share the lifetime of the owning `ZephyrResultSet*`. Never free them individually; `zephyr_result_free(result)` destroys the entire `Arena` in `O(slabs)` time.
3. **`zephyr_last_error(db)`:** Returns a pointer to an internal null-terminated string owned by `db`. Valid until the next API call on the same `ZephyrDB*` or `zephyr_db_close(db)`.
4. **`zephyr_result_to_json(db, result, out_len)`:** Returns a pointer to an internal buffer owned by `db` (populated via C++26 static reflection helpers). Valid until the next `zephyr_result_to_json` call on `db` or `zephyr_db_close(db)`.
5. **`zephyr_query_params(..., params, param_count)`:** The caller retains ownership of `params` and any `ZephyrSlice.ptr` buffers passed in; ZephyrDB only reads them synchronously during the call.

---

## 5. Unity C# Zero-GC / Zero-Serialization Read Pattern

In Unity (`unsafe` context enabled), mirror the C structs with `[StructLayout(LayoutKind.Sequential)]` and read directly from unmanaged arena memory without `Marshal.PtrToStructure` or managed `string` allocations on hot paths:

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ZephyrSlice
{
    public byte* ptr;
    public uint len;
    public uint reserved;

    public ReadOnlySpan<byte> AsSpan() => new ReadOnlySpan<byte>(ptr, (int)len);
    public string AsUtf8String() => len == 0 ? string.Empty : Encoding.UTF8.GetString(ptr, (int)len);
}

[StructLayout(LayoutKind.Explicit, Size = 24)]
public struct ZephyrValue
{
    [FieldOffset(0)] public int type;
    [FieldOffset(4)] public uint _pad;
    [FieldOffset(8)] public long i64;
    [FieldOffset(8)] public double f64;
    [FieldOffset(8)] public byte boolean;
    [FieldOffset(8)] public ZephyrSlice slice;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ZephyrColumnDesc
{
    public byte* name;
    public uint name_len;
    public int type;
}

[StructLayout(LayoutKind.Sequential)]
public unsafe struct ZephyrResultSet
{
    public ulong row_count;
    public ulong col_count;
    public ulong rows_affected;
    public ZephyrColumnDesc* columns;
    public ZephyrValue* values;
    public IntPtr _arena_handle;
    public uint status;
    public uint flags;
}

public static unsafe class ZephyrNative
{
    private const string LibName = "zephyrdb";

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    public static extern ZephyrResultSet* zephyr_query(IntPtr db, [MarshalAs(UnmanagedType.LPUTF8Str)] string sql);

    [DllImport(LibName, CallingConvention = CallingConvention.Cdecl)]
    public static extern void zephyr_result_free(ZephyrResultSet* result);

    // Zero-GC frame loop read example
    public static void ReadInventoryZeroGC(IntPtr db)
    {
        ZephyrResultSet* res = zephyr_query(db, "SELECT item_id, qty, tag FROM inventory");
        if (res == null) return;
        try
        {
            ulong cols = res->col_count;
            for (ulong r = 0; r < res->row_count; r++)
            {
                ZephyrValue* row = res->values + (r * cols);
                long itemId = row[0].i64;
                long qty    = row[1].i64;
                ReadOnlySpan<byte> tagUtf8 = row[2].slice.AsSpan(); // Zero managed heap allocation
                _ = (itemId, qty, tagUtf8.Length);
            }
        }
        finally
        {
            zephyr_result_free(res);
        }
    }
}
```

---

## 6. WASM LinearMemory Pointer Math (`wasm32`)

When compiled with Emscripten (`wasm32`), all pointers are 32-bit byte offsets into `Module.HEAPU8.buffer`. Using the `wasm32` offsets from Section 3:

```javascript
const decoder = new TextDecoder("utf-8");

function readResultSet(Module, resPtr) {
  const dv = new DataView(Module.HEAPU8.buffer);

  // ZephyrResultSet (48 bytes on wasm32)
  const rowCount   = Number(dv.getBigUint64(resPtr + 0, true));
  const colCount   = Number(dv.getBigUint64(resPtr + 8, true));
  const columnsPtr = dv.getUint32(resPtr + 24, true);
  const valuesPtr  = dv.getUint32(resPtr + 28, true);
  const status     = dv.getUint32(resPtr + 36, true);
  const flags      = dv.getUint32(resPtr + 40, true);

  // ZephyrColumnDesc is 12 bytes on wasm32
  const columns = [];
  for (let c = 0; c < colCount; c++) {
    const cPtr    = columnsPtr + c * 12;
    const namePtr = dv.getUint32(cPtr + 0, true);
    const nameLen = dv.getUint32(cPtr + 4, true);
    const type    = dv.getInt32(cPtr + 8, true);
    const name    = decoder.decode(Module.HEAPU8.subarray(namePtr, namePtr + nameLen));
    columns.push({ name, type });
  }

  // ZephyrValue is 24 bytes on wasm32
  const rows = [];
  for (let r = 0; r < rowCount; r++) {
    const row = new Array(colCount);
    for (let c = 0; c < colCount; c++) {
      const vPtr = valuesPtr + (r * colCount + c) * 24;
      const type = dv.getInt32(vPtr + 0, true);
      switch (type) {
        case 0: row[c] = null; break;                                   // ZEPHYR_TYPE_NULL
        case 1: row[c] = dv.getBigInt64(vPtr + 8, true); break;         // ZEPHYR_TYPE_I64
        case 2: row[c] = dv.getFloat64(vPtr + 8, true); break;          // ZEPHYR_TYPE_F64
        case 3: row[c] = dv.getUint8(vPtr + 8) !== 0; break;            // ZEPHYR_TYPE_BOOL
        case 4: {                                                       // ZEPHYR_TYPE_TEXT
          const sPtr = dv.getUint32(vPtr + 8, true);
          const sLen = dv.getUint32(vPtr + 12, true);
          row[c] = decoder.decode(Module.HEAPU8.subarray(sPtr, sPtr + sLen));
          break;
        }
        case 5: {                                                       // ZEPHYR_TYPE_BYTEA
          const bPtr = dv.getUint32(vPtr + 8, true);
          const bLen = dv.getUint32(vPtr + 12, true);
          row[c] = Module.HEAPU8.subarray(bPtr, bPtr + bLen);           // Zero-copy view
          break;
        }
      }
    }
    rows.push(row);
  }
  return { status, flags, columns, rows };
}
```
