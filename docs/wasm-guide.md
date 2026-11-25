# WebAssembly & Three.js Guide

ZephyrDB compiles to WebAssembly with **WASM SIMD128** (`-msimd128 -mrelaxed-simd`) and exposes a zero-copy TypeScript wrapper (`bindings/wasm/zephyrdb.ts`) that reads `ZephyrResultSet` cells directly from Emscripten's `HEAPU8` / `HEAPF64` linear memory.

---

## 1. Compiling with Emscripten

Ensure the Emscripten SDK (`emsdk`) is activated in your shell, then run the provided build script:

### Windows (PowerShell)

```powershell
.\bindings\wasm\build_wasm.ps1
```

### Linux / macOS (Bash)

```bash
./bindings/wasm/build_wasm.sh
```

Both scripts invoke `em++` with:
* `-std=c++26 -O3 -msimd128 -mrelaxed-simd -DZEPHYR_STATIC`
* `-sMODULARIZE=1 -sEXPORT_ES6=1 -sALLOW_MEMORY_GROWTH=1`
* Exported C-ABI functions (`_zephyr_db_open`, `_zephyr_exec`, `_zephyr_query`, `_zephyr_query_params`, `_zephyr_result_free`, `_zephyr_aggregate_count`, `_zephyr_aggregate_sum`, etc.)

The output ES module (`zephyrdb.mjs` + `zephyrdb.wasm`) is written to `bindings/wasm/dist/`.

---

## 2. TypeScript Usage (`zephyrdb.ts`)

`bindings/wasm/zephyrdb.ts` provides `ZephyrWasmDatabase` and `ZephyrWasmResult`.

* Numeric cells (`getInt64`, `getFloat64`, `getBoolean`) read directly from `DataView(HEAPU8.buffer)` or `HEAPF64`.
* `TEXT` / `BYTEA` cells expose `getBytesView(row, col)`, returning a zero-copy `Uint8Array` subarray over WASM linear memory.
* Always call `res.free()` when finished with a result set to release the native `Arena`.

```ts
import createZephyrModule from "./dist/zephyrdb.mjs";
import { ZephyrWasmDatabase, type ZephyrWasmModule } from "./zephyrdb";

const mod = (await createZephyrModule()) as ZephyrWasmModule;
const db = ZephyrWasmDatabase.open(mod, "/zephyr_mem");

db.exec("CREATE TABLE entities (id BIGINT PRIMARY KEY, name TEXT, x DOUBLE PRECISION, y DOUBLE PRECISION);");
db.exec("INSERT INTO entities VALUES (1, 'Turret_A', 12.5, -4.0), (2, 'Drone_B', 8.0, 19.25);");

const res = db.query("SELECT id, name, x, y FROM entities ORDER BY id;");
try {
  for (let r = 0; r < res.rowCount; r++) {
    const id = res.getInt64(r, 0);
    const name = res.getString(r, 1); // or res.getBytesView(r, 1) for zero-copy Uint8Array
    const x = res.getFloat64(r, 2);
    const y = res.getFloat64(r, 3);
    console.log({ id, name, x, y });
  }
} finally {
  res.free();
}

// O(1) aggregate check
const agg = db.query("SELECT COUNT(*), SUM(x) FROM entities;");
console.log("isO1FastPath:", agg.isO1FastPath, "count:", agg.getInt64(0, 0));
agg.free();

db.close();
```

---

## 3. Three.js / SolidJS Integration Pattern

In a Three.js `requestAnimationFrame` loop or SolidJS reactive store:
* Keep `ZephyrWasmDatabase` open as a singleton for the scene's lifetime (browser memory mode sets `enable_wal = 0`).
* Query `O(1)` aggregates or bounded spatial/ID slices per frame, read `getFloat64` / `getBytesView` in place, and call `res.free()` inside a `try / finally` block before the next frame.
