# ZephyrDB - Project Plan

**Mission:** A hyper-lightweight, embeddable database engineered in C++26, designed for high-performance game state (inventories, MMORPG shards, save systems). It executes Postgres queries natively, provides instant math via Augmented B-Trees, and exposes a pure C-ABI.

## Core Architecture (C++26)
- **Language:** C++26 (leveraging `std::rcu`, `std::simd`, static reflection, and `std::execution`).
- **Interface:** Strict C-ABI (`extern "C"`) for zero-overhead FFI bindings (Unity/C#, WebAssembly, Rust).
- **Concurrency:** Lock-free reads utilizing C++26 `std::rcu` and `std::hazard_pointer`.
- **Query Execution:** Vectorized column scanning using C++26 `std::simd`.
- **Serialization:** Zero-cost struct packing and JSON generation powered by C++26 compile-time reflection (`std::meta::info`).

## The Storage Engine: Augmented B-Trees
- B-Tree nodes cache aggregated metadata (COUNT, SUM) of their children.
- Math operations resolve in `O(1)` or `O(log N)` by reading the root/parent nodes instead of scanning rows.
- Updates propagate up the tree using `std::rcu` to avoid blocking concurrent game reads.

## Implementation Roadmap

### Phase 1: The Modern Core
- Set up the C++26 toolchain (latest Clang/GCC).
- Implement the Arena Allocator and Augmented B-Tree structure.
- Integrate `std::rcu` for lock-free read access.
- Build the initial C-ABI bindings and prove memory layout stability.

### Phase 2: Vectorized Execution & Parsing
- Write a lightweight, recursive descent Postgres SQL parser (supporting `WITH`, `JOIN`, `RETURNING`).
- Implement the execution engine using `std::simd` to process query filters and hash-joins in vectorized blocks.
- Use C++26 static reflection to automate query result serialization back across the C-ABI.

### Phase 3: Embedded Persistence
- Implement the Write-Ahead Log (WAL) using `std::execution` for asynchronous, non-blocking disk I/O.
- Memory-map the compacted state files.
- Ensure ACID compliance for local save-game reliability.

### Phase 4: Interoperability Ecosystem
- Compile the core to WebAssembly (WASM) for browser-based Three.js/SolidJS game clients.
- Generate the native Unity (C#) `DllImport` wrapper.
- Optimize the memory handoff (zero-copy reads directly from the engine's Arena).

### Phase 5: The MMORPG Layer (Raft & Postgres Wire Protocol)
- Embed a lightweight Raft consensus algorithm utilizing `std::execution` senders/receivers for networking.
- Implement the PostgreSQL wire protocol over a lightweight TCP socket.
- Support spinning up seamless Read Replicas that subscribe to the leader's WAL stream.