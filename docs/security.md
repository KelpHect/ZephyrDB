# Security & Encryption at Rest

ZephyrDB provides **built-in authenticated encryption at rest (AEAD)** and **strict synchronous durability** with zero external cryptographic dependencies. Every byte written to disk—both the Write-Ahead Log (`zephyr.wal`) and compacted snapshots (`zephyr.snap`)—can be encrypted and tamper-protected using pure C++26 **RFC 8439 ChaCha20-Poly1305** and **HKDF-SHA256**.

---

## 1. Cryptographic Design

### ChaCha20-Poly1305 AEAD (RFC 8439)
ZephyrDB implements the IETF **ChaCha20-Poly1305** Authenticated Encryption with Associated Data (AEAD) construction directly in `zephyr::storage::crypto`:
- **Confidentiality:** 256-bit ChaCha20 stream cipher with 96-bit nonces and a 32-bit block counter (block `0` derives the one-time Poly1305 key; encryption begins at block `1`).
- **Integrity & Authenticity:** 128-bit Poly1305 authentication tag computed over the Associated Authenticated Data (AAD), ciphertext, and 64-bit little-endian lengths.
- **Constant-Time Verification:** Tag verification runs in constant time to prevent timing side-channel attacks. Any single-bit modification to headers, ciphertext, or tags is rejected immediately.
- **Self-Test Verified:** Validated against the official RFC 8439 Section 2.8.2 test vector (`zephyr::storage::crypto::self_test()`).

### HKDF-SHA256 Key Derivation (Domain Separation)
You pass a single master secret (passphrase or 32-byte binary key) to `zephyr_db_open_encrypted()` or `--encryption-key`. ZephyrDB never uses the raw secret directly for encryption. Instead, it runs **HKDF-SHA256** (RFC 5869 Extract + Expand) with distinct domain-separation labels to derive two independent 256-bit subkeys:

| Subkey | HKDF Info Label | Purpose |
|---|---|---|
| `wal_key` (32 bytes) | `"zephyr-wal-v1"` | Encrypts and authenticates WAL records (`ZWAE`) |
| `snap_key` (32 bytes) | `"zephyr-snap-v1"` | Encrypts and authenticates compacted snapshots (`ZSNE`) |

### On-Disk Formats
- **Encrypted WAL (`ZWAE`):**
  ```text
  [magic:4 ("ZWAE")][nonce_salt:4][lsn:8][op:1][len:4][ciphertext:len][tag:16]
  ```
  - **Nonce (12 bytes):** `nonce_salt (4) || lsn (8)` — unique per WAL record.
  - **AAD (17 bytes):** `magic (4) || lsn (8) || op (1) || len (4)` — binds the record framing and LSN to the authentication tag so records cannot be reordered or truncated silently.
- **Encrypted Snapshot (`ZSNE`):**
  ```text
  [magic:4 ("ZSNE")][nonce:12][len:8][ciphertext:len][tag:16]
  ```
  - Written atomically via temp-file + OS flush + atomic rename.
- **Fail-Closed Authentication:** Opening an encrypted database with a wrong key, opening an encrypted database without a key, opening a plaintext database with a key, or loading a tampered WAL/snapshot file causes `zephyr_db_open_encrypted()` to fail closed and return `nullptr` (with `ZEPHYR_ERR_AUTH` semantics).

---

## 2. Opening an Encrypted Database

### C / C++ Embedded API
```c
#include "zephyr/c_abi.h"
#include <string.h>

ZephyrConfig cfg = zephyr_default_config();
cfg.data_dir = "./secure_data";
cfg.enable_wal = 1;

const char* secret = getenv("ZEPHYR_MASTER_KEY");
ZephyrDB* db = zephyr_db_open_encrypted(
    &cfg,
    (const uint8_t*)secret,
    (uint32_t)strlen(secret)
);
if (!db) {
    // Wrong key, tampered files, or missing key
    fprintf(stderr, "Database authentication failed!\n");
    return 1;
}
```

### Standalone Server (`zephyrdb_server`)
Pass `--encryption-key <secret>` when starting `zephyrdb_server`:
```bash
zephyrdb_server \
  --data-dir ./prod_data \
  --pgwire-port 5432 \
  --encryption-key "$ZEPHYR_MASTER_KEY"
```

> **Production Tip:** Always inject `--encryption-key` from a secrets manager or environment variable rather than hardcoding it in scripts or source control.

---

## 3. Strict Durability (`zephyr_exec_sync`)

By default, `zephyr_exec()` uses asynchronous group-commit WAL batching via the background **WAL Guardian** agent for maximum throughput (ideal for game state, telemetry, and high-frequency updates).

For **security-critical and financial writes**—such as user registrations, password resets, payment ledger transfers, and in-game shop purchases—use **`zephyr_exec_sync()`**:
- Blocks until the WAL Guardian has encrypted, written, and OS-flushed (`FlushFileBuffers` / `fsync`) the record to physical disk.
- Guarantees zero data loss even on sudden power loss or process crash.

```c
uint64_t affected = 0;
int32_t rc = zephyr_exec_sync(
    db,
    "INSERT INTO payments (tx_id, user_id, amount_cents, status) "
    "VALUES (9001, 42, 1999, 'SETTLED')",
    &affected
);
if (rc != ZEPHYR_OK) {
    fprintf(stderr, "Durable commit failed: %s\n", zephyr_last_error(db));
}
```

---

## 4. Recommended Patterns: Login, Payments & Shop

### User Authentication & Login
- Store salted password hashes (e.g., Argon2id or scrypt computed in your application layer) in a `TEXT` or `BYTEA` column—never plaintext passwords.
- Use **parameterized prepared statements** (`zephyr_prepare` + `zephyr_stmt_query` / `zephyr_stmt_exec`) to prevent SQL injection and unlock ZephyrDB's O(1) primary-key lookup fastpath.
- Commit account creation, credential updates, and session revocations with `zephyr_exec_sync()`.

```sql
CREATE TABLE users (
    user_id BIGINT PRIMARY KEY,
    email TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    mfa_secret TEXT
);
```

### Payments & Virtual Shop Purchases
- Wrap balance debits, inventory grants, and ledger inserts inside an explicit **`BEGIN` / `COMMIT`** transaction so partial failures roll back atomically.
- Follow with `zephyr_checkpoint()` periodically or use `zephyr_exec_sync()` for single-statement ledger appends.

```sql
BEGIN;
UPDATE wallets SET balance_cents = balance_cents - 500 WHERE user_id = 42;
INSERT INTO shop_orders (order_id, user_id, item_id, price_cents) VALUES (10001, 42, 7, 500);
COMMIT;
```

### Defense-in-Depth Checklist
1. **Encryption at Rest:** Always open production databases with `zephyr_db_open_encrypted()` / `--encryption-key`.
2. **Parameterized Queries:** Bind all user-supplied values via `$1, $2, ...` parameters instead of string concatenation.
3. **Sync Durability for Money/Auth:** Use `zephyr_exec_sync()` for login, billing, and shop mutations.
4. **Network Isolation:** Bind `zephyrdb_server` behind a private VPC or TLS-terminating reverse proxy when exposing PGWire or Studio.
