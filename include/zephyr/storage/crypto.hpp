#ifndef ZEPHYR_STORAGE_CRYPTO_HPP
#define ZEPHYR_STORAGE_CRYPTO_HPP

#include <cstddef>
#include <cstdint>

namespace zephyr::storage::crypto {

/// Compute SHA-256 digest of `data[0..len-1]` into `out[0..31]`.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) noexcept;

/// Compute HMAC-SHA256 of `data[0..len-1]` under `key[0..key_len-1]` into `out[0..31]`.
void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t len,
                 uint8_t out[32]) noexcept;

/// Derive independent 32-byte WAL and snapshot encryption keys from `master_key`
/// using HKDF-SHA256 (RFC 5869) with distinct info labels "zephyr-wal-v1" and "zephyr-snap-v1".
void hkdf_derive_keys(const uint8_t* master_key, size_t key_len,
                      uint8_t out_wal_key[32],
                      uint8_t out_snap_key[32]) noexcept;

/// RFC 8439 ChaCha20-Poly1305 AEAD encryption.
/// Supports `pt_len == 0` and in-place operation (`ct_out == pt`).
void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* pt, size_t pt_len,
                  uint8_t* ct_out, uint8_t tag_out[16]) noexcept;

/// RFC 8439 ChaCha20-Poly1305 AEAD decryption with constant-time 16-byte tag verification.
/// Returns true on authentication success; returns false without writing plaintext on failure.
bool aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ct, size_t ct_len,
                  const uint8_t tag[16], uint8_t* pt_out) noexcept;

/// Fill `out[0..len-1]` with cryptographically secure random bytes
/// (BCryptGenRandom on Windows, /dev/urandom on POSIX, std::random_device fallback).
void random_bytes(uint8_t* out, size_t len);

/// Run self-tests verifying RFC 8439 Section 2.8.2 AEAD test vector,
/// tamper rejection, SHA-256, and HKDF key derivation.
bool self_test() noexcept;

} // namespace zephyr::storage::crypto

#endif // ZEPHYR_STORAGE_CRYPTO_HPP
