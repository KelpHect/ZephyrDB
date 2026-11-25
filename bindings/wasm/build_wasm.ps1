$ErrorActionPreference = "Stop"

$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$OutDir = Join-Path $PSScriptRoot "dist"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Sources = @(
    "src/core/arena.cpp",
    "src/core/rcu.cpp",
    "src/core/augmented_btree.cpp",
    "src/storage/wal.cpp",
    "src/storage/mmap_store.cpp",
    "src/sql/lexer.cpp",
    "src/sql/parser.cpp",
    "src/sql/simd_eval.cpp",
    "src/sql/executor.cpp",
    "src/net/raft.cpp",
    "src/net/pgwire.cpp",
    "src/c_abi.cpp"
) | ForEach-Object { Join-Path $Root $_ }

$ExportedFns = '["_malloc","_free","_zephyr_default_config","_zephyr_db_open","_zephyr_db_close","_zephyr_exec","_zephyr_query","_zephyr_query_params","_zephyr_result_free","_zephyr_aggregate_count","_zephyr_aggregate_sum","_zephyr_checkpoint","_zephyr_last_error","_zephyr_result_to_json","_zephyr_raft_start","_zephyr_raft_stop","_zephyr_pgwire_start","_zephyr_pgwire_stop"]'

& em++ -std=c++26 -O3 -msimd128 -mrelaxed-simd -DZEPHYR_STATIC `
    -I"$Root\include" `
    @Sources `
    -sMODULARIZE=1 `
    -sEXPORT_ES6=1 `
    -sALLOW_MEMORY_GROWTH=1 `
    -sEXPORTED_FUNCTIONS="$ExportedFns" `
    -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' `
    -o "$OutDir\zephyrdb.mjs"

Write-Host "Built WASM artifacts in $OutDir"
