# ZephyrDB C++26 Build & Toolchain Specification

This document covers supported C++26 compilers, flags configured in `CMakeLists.txt`, the `include/zephyr/compat/cpp26.hpp` feature-detection bridge, platform networking/linking, and CMake + Ninja build recipes for Windows, Linux, and WebAssembly.

---

## 1. Compiler Versions & Flags

`CMakeLists.txt` requires CMake `3.20+` with `CMAKE_CXX_STANDARD_REQUIRED ON` and `CMAKE_CXX_EXTENSIONS OFF`.

| Toolchain | Minimum Version | Core C++26 & SIMD Flags | Notes |
| :--- | :--- | :--- | :--- |
| **GCC** | `15` / `16+` | `-std=c++26 -mavx2` *(+ `-freflection -fexpansion-statements` on GCC 16+ with P2996)* | `CMakeLists.txt` applies `-std=c++26 -Wall -Wextra -Wno-unused-parameter` and `-O3 -mavx2` on `Release` x86_64/AMD64. |
| **Clang** | `19+` | `-std=c++26 -mavx2` | Uses `-std=c++26 -Wall -Wextra -Wno-unused-parameter` and `-O3 -mavx2` on `Release` x86_64/AMD64. |
| **MSVC** | `19.52+` (VS 2022/2026) | `/std:c++latest /permissive- /utf-8 /arch:AVX2` | `CMakeLists.txt` also passes `/W4 /wd4100 /wd4201 /wd4324`, `/O2`, and defines `_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN`. |
| **Emscripten** | `emcc` / `em++` (Clang 19+ backend) | `-std=c++26 -O3 -msimd128 -mrelaxed-simd -sALLOW_MEMORY_GROWTH -sEXPORTED_FUNCTIONS=...` | `CMakeLists.txt` passes `-std=c++26 -Wall -Wextra -msimd128 -mrelaxed-simd` and `-O3` on `Release`. Linker flags (`-sALLOW_MEMORY_GROWTH`, `-sEXPORTED_FUNCTIONS=...`) are passed at the WASM link step. |

> **Symbol Visibility:** On non-WASM builds, the shared library target `zephyrdb` compiles with `ZEPHYR_BUILD_DLL`, `C_VISIBILITY_PRESET default`, and `CXX_VISIBILITY_PRESET hidden`, exporting only `ZEPHYR_API` C-ABI symbols. Static library `zephyrdb_core` defines `ZEPHYR_STATIC` `PUBLIC`.

---

## 2. C++26 Compatibility Bridge (`include/zephyr/compat/cpp26.hpp`)

Because standard library support for C++26 features varies across toolchains, `include/zephyr/compat/cpp26.hpp` probes native headers and feature-test macros via `__has_include`:

| C++26 Feature | Native Header & Macro Guard | Internal Macro | Fallback Implementation (`zephyr::cpp26::*`) |
| :--- | :--- | :--- | :--- |
| **RCU (P2545)** | `__has_include(<rcu>) && defined(__cpp_lib_rcu)` | `ZEPHYR_NATIVE_RCU` | `zephyr::cpp26::rcu`: `rcu_domain` with 256 cache-line-aligned (`alignas(64)`) `reader_slot`s, `scoped_read_lock`, `retire()`, `poll_reclaim()`, `synchronize()`, `drain_all()`. |
| **SIMD (P1928)** | `__has_include(<simd>) && defined(__cpp_lib_simd)` | `ZEPHYR_NATIVE_SIMD` | `zephyr::cpp26::simd`: `filter_chunk_i64`, `filter_chunk_f64`, `reduce_sum_i64` using AVX2 (`_mm256_*` when `ZEPHYR_HAS_X86_SIMD` & `__AVX2__`), WASM SIMD128 (`wasm_i64x2_*` / `wasm_f64x2_*` when `ZEPHYR_HAS_WASM_SIMD`), or scalar fallback. |
| **Execution (P2300)** | `__has_include(<execution>) && defined(__cpp_lib_senders_receivers)` | `ZEPHYR_NATIVE_EXECUTION` | `zephyr::cpp26::execution`: `thread_pool_scheduler`, `task_sender` with `.then()`, `schedule_task()`, `sync_wait()`, `start_detached()`. |
| **Static Reflection (P2996)** | `__has_include(<meta>) && defined(__cpp_impl_reflection)` | `ZEPHYR_NATIVE_REFLECTION` | `zephyr::cpp26::reflection`: `field_descriptor`, `make_field`, `for_each_field`, `to_json`, plus `ZEPHYR_FIELD` / `ZEPHYR_REFLECT` registration macros. |

---

## 3. Networking & Linking Matrix

Raft Diplomat (`src/net/raft.cpp`) and PGWire (`src/net/pgwire.cpp`) adapt per platform in `CMakeLists.txt`:

| Platform | Socket Backend | Linked Libraries (`CMakeLists.txt`) | Built Targets |
| :--- | :--- | :--- | :--- |
| **Windows** | Winsock2 (`ws2_32`) | `ws2_32` (`PUBLIC` on `zephyrdb_core`, `PRIVATE` on `zephyrdb`) | `zephyrdb_core`, `zephyrdb` (DLL), `zephyrdb_server`, `zephyrdb_tests`, `zephyrdb_bench` |
| **Linux / macOS** | POSIX sockets | `Threads::Threads` (`-pthread`) | `zephyrdb_core`, `zephyrdb` (`.so`/`.dylib`), `zephyrdb_server`, `zephyrdb_tests`, `zephyrdb_bench` |
| **WASM (`EMSCRIPTEN`)** | Stub / WebSocket bridge mode | *(no `ws2_32` or `Threads::Threads`)* | `zephyrdb_core` only (shared lib, server, tests, and benchmarks are gated off by `NOT EMSCRIPTEN`) |

---

## 4. Step-by-Step Build Commands (CMake + Ninja)

### Windows (MSVC 19.52+ / x64 Native Tools Command Prompt)

```powershell
cmake -B build-win -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl
cmake --build build-win --parallel
ctest --test-dir build-win --output-on-failure
```

### Linux (GCC 15/16 or Clang 19+)

```bash
cmake -B build-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

*(For GCC 16 with native reflection flags, pass `-DCMAKE_CXX_FLAGS="-freflection -fexpansion-statements"`.)*

### WebAssembly (Emscripten `emcmake` + Ninja)

```bash
emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm --target zephyrdb_core --parallel

# Link zephyrdb_core into a browser/Node WASM module exporting the C-ABI
em++ build-wasm/libzephyrdb_core.a -O3 -msimd128 -mrelaxed-simd \
  -sALLOW_MEMORY_GROWTH=1 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME="createZephyrDB" \
  -sEXPORTED_FUNCTIONS="['_zephyr_default_config','_zephyr_db_open','_zephyr_db_close','_zephyr_exec','_zephyr_query','_zephyr_query_params','_zephyr_result_free','_zephyr_aggregate_count','_zephyr_aggregate_sum','_zephyr_checkpoint','_zephyr_last_error','_zephyr_result_to_json','_malloc','_free']" \
  -o zephyrdb.js
```
