# ZephyrDB Agent Architecture

To ensure zero frame drops when embedded in a game engine like Unity or Three.js, ZephyrDB does not use a traditional monolithic blocking server model. Instead, it utilizes an internal **Actor/Agent Architecture** powered by C++26 `std::execution` (Senders/Receivers). 

These lightweight, autonomous agents run asynchronously, handling maintenance, networking, and disk I/O completely independent of the main read/write execution thread.

## 1. Internal System Agents

### The WAL Guardian (Disk I/O Agent)
* **Role:** Ensures data durability without blocking the game thread.
* **Mechanism:** When a transaction commits, it is pushed to a lock-free memory ring buffer. The WAL Guardian agent wakes up, batches these transactions using C++26 `std::simd` optimizations, and flushes them to the physical Write-Ahead Log (WAL) on disk via non-blocking I/O. 

### The RCU Reaper (Memory Agent)
* **Role:** Safely reclaims memory from the Augmented B-Tree.
* **Mechanism:** Because read queries use `std::rcu` for lock-free access, modified or deleted tree nodes cannot be immediately destroyed. The RCU Reaper monitors active grace periods and gracefully deallocates old memory blocks back to the Arena Allocator once all active read queries have finished.

### The Raft Diplomat (Consensus & Network Agent)
* **Role:** Manages cluster state for MMORPG servers or cloud web apps.
* **Mechanism:** Runs entirely on a background execution context. It sends UDP/TCP heartbeats to other ZephyrDB nodes, triggers leader elections if a node goes down, and streams WAL byte-deltas to connected Read Replicas.

### The Compactor (Optimization Agent)
* **Role:** Keeps the database footprint tiny.
* **Mechanism:** Periodically wakes up to merge the append-only WAL into the compacted, memory-mapped main state file, ensuring the database doesn't infinitely grow in size during a long gaming session.

## 2. External / AI Agent Integration

Because ZephyrDB is extremely fast, local, and embeddable, it is uniquely positioned to act as the **Local Memory Store for AI Agents and NPCs** in modern games.

* **Instant NPC Memory:** Instead of standard AI memory vectors, LLM-powered NPCs can query ZephyrDB for instant aggregate stats (e.g., `SELECT COUNT(*) FROM player_crimes WHERE location = 'town_square'`) to dynamically adjust their behavior. The Augmented B-Tree ensures this query takes $O(1)$ time, allowing dozens of AI agents to query the database every frame without lagging the engine.
* **Agent State Persistence:** AI agent goals, sub-tasks, and dialogue trees can be stored as serialized MessagePack structs in ZephyrDB, instantly hydrated back into the game engine via the C-ABI.