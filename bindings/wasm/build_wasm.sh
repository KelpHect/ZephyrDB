#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUT_DIR="$SCRIPT_DIR/dist"
mkdir -p "$OUT_DIR"

SOURCES=(
  "$ROOT/src/core/arena.cpp"
  "$ROOT/src/core/rcu.cpp"
  "$ROOT/src/core/augmented_btree.cpp"
  "$ROOT/src/storage/wal.cpp"
  "$ROOT/src/storage/mmap_store.cpp"
  "$ROOT/src/sql/lexer.cpp"
  "$ROOT/src/sql/parser.cpp"
  "$ROOT/src/sql/simd_eval.cpp"
  "$ROOT/src/sql/executor.cpp"
  "$ROOT/src/net/raft.cpp"
  "$ROOT/src/net/pgwire.cpp"
  "$ROOT/src/c_abi.cpp"
)

EXPORTED_FNS='["_malloc","_free","_zephyr_default_config","_zephyr_db_open","_zephyr_db_close","_zephyr_exec","_zephyr_query","_zephyr_query_params","_zephyr_result_free","_zephyr_aggregate_count","_zephyr_aggregate_sum","_zephyr_checkpoint","_zephyr_last_error","_zephyr_result_to_json","_zephyr_raft_start","_zephyr_raft_stop","_zephyr_pgwire_start","_zephyr_pgwire_stop"]'

em++ -std=c++26 -O3 -msimd128 -mrelaxed-simd -DZEPHYR_STATIC \
  -I"$ROOT/include" \
  "${SOURCES[@]}" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sEXPORTED_FUNCTIONS="$EXPORTED_FNS" \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
  -o "$OUT_DIR/zephyrdb.mjs"

echo "Built WASM artifacts in $OUT_DIR"
