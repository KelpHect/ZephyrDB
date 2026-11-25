#include "zephyr/storage/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <random>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#elif !defined(__EMSCRIPTEN__)
  #include <cstdio>
#endif

namespace zephyr::storage::crypto {

namespace {

inline uint32_t rotl32(uint32_t v, unsigned shift) noexcept {
    return (v << shift) | (v >> (32u - shift));
}

inline uint32_t rotr32(uint32_t v, unsigned shift) noexcept {
    return (v >> shift) | (v << (32u - shift));
}

inline uint32_t load_le32(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void store_le32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void store_le64(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFFu);
    }
}

inline uint32_t load_be32(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

inline void store_be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(v & 0xFFu);
}

inline void secure_zero(void* ptr, size_t len) noexcept {
    volatile uint8_t* p = static_cast<volatile uint8_t*>(ptr);
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
}

// ============================================================================
// SHA-256 (FIPS 180-4)
// ============================================================================

constexpr std::array<uint32_t, 64> SHA256_K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct Sha256Ctx {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    uint64_t total_bits = 0;
    uint8_t block[64]{};
    size_t block_len = 0;

    void compress(const uint8_t blk[64]) noexcept {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = load_be32(blk + i * 4);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];

        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + S1 + ch + SHA256_K[static_cast<size_t>(i)] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    void update(const uint8_t* data, size_t len) noexcept {
        if (!data || len == 0) {
            return;
        }
        total_bits += static_cast<uint64_t>(len) * 8u;
        size_t offset = 0;
        if (block_len > 0) {
            const size_t take = std::min(64 - block_len, len);
            std::memcpy(block + block_len, data, take);
            block_len += take;
            offset += take;
            if (block_len == 64) {
                compress(block);
                block_len = 0;
            }
        }
        while (offset + 64 <= len) {
            compress(data + offset);
            offset += 64;
        }
        if (offset < len) {
            block_len = len - offset;
            std::memcpy(block, data + offset, block_len);
        }
    }

    void finish(uint8_t out[32]) noexcept {
        block[block_len++] = 0x80u;
        if (block_len > 56) {
            std::memset(block + block_len, 0, 64 - block_len);
            compress(block);
            block_len = 0;
        }
        std::memset(block + block_len, 0, 56 - block_len);
        for (int i = 0; i < 8; ++i) {
            block[56 + i] = static_cast<uint8_t>((total_bits >> ((7 - i) * 8)) & 0xFFu);
        }
        compress(block);
        for (int i = 0; i < 8; ++i) {
            store_be32(out + i * 4, state[i]);
        }
        secure_zero(block, sizeof(block));
    }
};

// ============================================================================
// ChaCha20 (RFC 8439 Section 2.3 / 2.4)
// ============================================================================

inline void chacha20_quarter_round(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) noexcept {
    a += b; d ^= a; d = rotl32(d, 16);
    c += d; b ^= c; b = rotl32(b, 12);
    a += b; d ^= a; d = rotl32(d, 8);
    c += d; b ^= c; b = rotl32(b, 7);
}

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12],
                    uint8_t out[64]) noexcept {
    uint32_t state[16];
    state[0] = 0x61707865u;
    state[1] = 0x3320646eu;
    state[2] = 0x79622d32u;
    state[3] = 0x6b206574u;
    for (int i = 0; i < 8; ++i) {
        state[4 + i] = load_le32(key + i * 4);
    }
    state[12] = counter;
    state[13] = load_le32(nonce + 0);
    state[14] = load_le32(nonce + 4);
    state[15] = load_le32(nonce + 8);

    uint32_t working[16];
    std::memcpy(working, state, sizeof(state));

    for (int i = 0; i < 10; ++i) {
        // Column rounds
        chacha20_quarter_round(working[0], working[4], working[8],  working[12]);
        chacha20_quarter_round(working[1], working[5], working[9],  working[13]);
        chacha20_quarter_round(working[2], working[6], working[10], working[14]);
        chacha20_quarter_round(working[3], working[7], working[11], working[15]);
        // Diagonal rounds
        chacha20_quarter_round(working[0], working[5], working[10], working[15]);
        chacha20_quarter_round(working[1], working[6], working[11], working[12]);
        chacha20_quarter_round(working[2], working[7], working[8],  working[13]);
        chacha20_quarter_round(working[3], working[4], working[9],  working[14]);
    }

    for (int i = 0; i < 16; ++i) {
        store_le32(out + i * 4, working[i] + state[i]);
    }
}

void chacha20_xor(const uint8_t key[32], uint32_t initial_counter, const uint8_t nonce[12],
                  const uint8_t* in, size_t len, uint8_t* out) noexcept {
    if (len == 0 || !in || !out) {
        return;
    }
    uint8_t keystream[64];
    uint32_t counter = initial_counter;
    size_t offset = 0;
    while (offset < len) {
        chacha20_block(key, counter++, nonce, keystream);
        const size_t chunk = std::min<size_t>(64, len - offset);
        for (size_t i = 0; i < chunk; ++i) {
            out[offset + i] = static_cast<uint8_t>(in[offset + i] ^ keystream[i]);
        }
        offset += chunk;
    }
    secure_zero(keystream, sizeof(keystream));
}

// ============================================================================
// Poly1305 (RFC 8439 Section 2.5, 26-bit limb implementation)
// ============================================================================

struct Poly1305Ctx {
    uint32_t r[5]{};
    uint32_t h[5]{};
    uint32_t pad[4]{};
    uint8_t buffer[16]{};
    size_t leftover = 0;

    void init(const uint8_t key[32]) noexcept {
        // r &= 0x0ffffffc0ffffffc0ffffffc0fffffff in 26-bit limbs
        r[0] = (load_le32(key + 0))       & 0x3ffffffu;
        r[1] = (load_le32(key + 3) >> 2)  & 0x3ffff03u;
        r[2] = (load_le32(key + 6) >> 4)  & 0x3ffc0ffu;
        r[3] = (load_le32(key + 9) >> 6)  & 0x3f03fffu;
        r[4] = (load_le32(key + 12) >> 8) & 0x00fffffu;

        h[0] = h[1] = h[2] = h[3] = h[4] = 0;

        pad[0] = load_le32(key + 16);
        pad[1] = load_le32(key + 20);
        pad[2] = load_le32(key + 24);
        pad[3] = load_le32(key + 28);

        leftover = 0;
    }

    void process_block(const uint8_t m[16], bool is_final_partial) noexcept {
        const uint32_t hibit = is_final_partial ? 0u : (1u << 24);

        const uint32_t r0 = r[0];
        const uint32_t r1 = r[1];
        const uint32_t r2 = r[2];
        const uint32_t r3 = r[3];
        const uint32_t r4 = r[4];

        const uint32_t s1 = r1 * 5u;
        const uint32_t s2 = r2 * 5u;
        const uint32_t s3 = r3 * 5u;
        const uint32_t s4 = r4 * 5u;

        uint32_t h0 = h[0] + ((load_le32(m + 0))       & 0x3ffffffu);
        uint32_t h1 = h[1] + ((load_le32(m + 3) >> 2)  & 0x3ffffffu);
        uint32_t h2 = h[2] + ((load_le32(m + 6) >> 4)  & 0x3ffffffu);
        uint32_t h3 = h[3] + ((load_le32(m + 9) >> 6)  & 0x3ffffffu);
        uint32_t h4 = h[4] + ((load_le32(m + 12) >> 8) | hibit);

        uint64_t d0 = static_cast<uint64_t>(h0) * r0 +
                      static_cast<uint64_t>(h1) * s4 +
                      static_cast<uint64_t>(h2) * s3 +
                      static_cast<uint64_t>(h3) * s2 +
                      static_cast<uint64_t>(h4) * s1;
        uint64_t d1 = static_cast<uint64_t>(h0) * r1 +
                      static_cast<uint64_t>(h1) * r0 +
                      static_cast<uint64_t>(h2) * s4 +
                      static_cast<uint64_t>(h3) * s3 +
                      static_cast<uint64_t>(h4) * s2;
        uint64_t d2 = static_cast<uint64_t>(h0) * r2 +
                      static_cast<uint64_t>(h1) * r1 +
                      static_cast<uint64_t>(h2) * r0 +
                      static_cast<uint64_t>(h3) * s4 +
                      static_cast<uint64_t>(h4) * s3;
        uint64_t d3 = static_cast<uint64_t>(h0) * r3 +
                      static_cast<uint64_t>(h1) * r2 +
                      static_cast<uint64_t>(h2) * r1 +
                      static_cast<uint64_t>(h3) * r0 +
                      static_cast<uint64_t>(h4) * s4;
        uint64_t d4 = static_cast<uint64_t>(h0) * r4 +
                      static_cast<uint64_t>(h1) * r3 +
                      static_cast<uint64_t>(h2) * r2 +
                      static_cast<uint64_t>(h3) * r1 +
                      static_cast<uint64_t>(h4) * r0;

        uint32_t c = static_cast<uint32_t>(d0 >> 26); h0 = static_cast<uint32_t>(d0) & 0x3ffffffu;
        d1 += c; c = static_cast<uint32_t>(d1 >> 26); h1 = static_cast<uint32_t>(d1) & 0x3ffffffu;
        d2 += c; c = static_cast<uint32_t>(d2 >> 26); h2 = static_cast<uint32_t>(d2) & 0x3ffffffu;
        d3 += c; c = static_cast<uint32_t>(d3 >> 26); h3 = static_cast<uint32_t>(d3) & 0x3ffffffu;
        d4 += c; c = static_cast<uint32_t>(d4 >> 26); h4 = static_cast<uint32_t>(d4) & 0x3ffffffu;
        h0 += c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
        h1 += c;

        h[0] = h0;
        h[1] = h1;
        h[2] = h2;
        h[3] = h3;
        h[4] = h4;
    }

    void update(const uint8_t* m, size_t bytes) noexcept {
        if (!m || bytes == 0) {
            return;
        }
        size_t offset = 0;
        if (leftover > 0) {
            const size_t want = std::min(16 - leftover, bytes);
            std::memcpy(buffer + leftover, m, want);
            leftover += want;
            offset += want;
            if (leftover == 16) {
                process_block(buffer, false);
                leftover = 0;
            }
        }
        while (offset + 16 <= bytes) {
            process_block(m + offset, false);
            offset += 16;
        }
        if (offset < bytes) {
            leftover = bytes - offset;
            std::memcpy(buffer, m + offset, leftover);
        }
    }

    void pad16(size_t data_len) noexcept {
        const size_t rem = data_len & 15u;
        if (rem != 0) {
            static constexpr uint8_t zeros[16]{};
            update(zeros, 16u - rem);
        }
    }

    void finish(uint8_t mac[16]) noexcept {
        if (leftover > 0) {
            buffer[leftover] = 1u;
            for (size_t i = leftover + 1; i < 16; ++i) {
                buffer[i] = 0u;
            }
            process_block(buffer, true);
        }

        uint32_t h0 = h[0];
        uint32_t h1 = h[1];
        uint32_t h2 = h[2];
        uint32_t h3 = h[3];
        uint32_t h4 = h[4];

        uint32_t c = h1 >> 26; h1 &= 0x3ffffffu;
        h2 += c; c = h2 >> 26; h2 &= 0x3ffffffu;
        h3 += c; c = h3 >> 26; h3 &= 0x3ffffffu;
        h4 += c; c = h4 >> 26; h4 &= 0x3ffffffu;
        h0 += c * 5u; c = h0 >> 26; h0 &= 0x3ffffffu;
        h1 += c;

        // Compute h + (-p) = h + 5 - 2^130
        uint32_t g0 = h0 + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
        uint32_t g1 = h1 + c;  c = g1 >> 26; g1 &= 0x3ffffffu;
        uint32_t g2 = h2 + c;  c = g2 >> 26; g2 &= 0x3ffffffu;
        uint32_t g3 = h3 + c;  c = g3 >> 26; g3 &= 0x3ffffffu;
        uint32_t g4 = h4 + c - (1u << 26);

        // Select h if h < p (g4 MSB == 1), else g
        uint32_t mask = (g4 >> 31) - 1u;
        g0 &= mask;
        g1 &= mask;
        g2 &= mask;
        g3 &= mask;
        g4 &= mask;
        mask = ~mask;
        h0 = (h0 & mask) | g0;
        h1 = (h1 & mask) | g1;
        h2 = (h2 & mask) | g2;
        h3 = (h3 & mask) | g3;
        h4 = (h4 & mask) | g4;

        // Pack into 4 x 32-bit words (mod 2^128)
        h0 = (h0)       | (h1 << 26);
        h1 = (h1 >> 6)  | (h2 << 20);
        h2 = (h2 >> 12) | (h3 << 14);
        h3 = (h3 >> 18) | (h4 << 8);

        // mac = (h + pad) mod 2^128
        uint64_t f = static_cast<uint64_t>(h0) + pad[0];
        h0 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h1) + pad[1] + (f >> 32);
        h1 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h2) + pad[2] + (f >> 32);
        h2 = static_cast<uint32_t>(f);
        f = static_cast<uint64_t>(h3) + pad[3] + (f >> 32);
        h3 = static_cast<uint32_t>(f);

        store_le32(mac + 0, h0);
        store_le32(mac + 4, h1);
        store_le32(mac + 8, h2);
        store_le32(mac + 12, h3);

        secure_zero(this, sizeof(*this));
    }
};

void compute_aead_tag(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t* aad, size_t aad_len,
                      const uint8_t* ct, size_t ct_len,
                      uint8_t tag_out[16]) noexcept {
    uint8_t poly_key_block[64];
    chacha20_block(key, 0u, nonce, poly_key_block);

    Poly1305Ctx poly;
    poly.init(poly_key_block);
    secure_zero(poly_key_block, sizeof(poly_key_block));

    if (aad_len > 0 && aad != nullptr) {
        poly.update(aad, aad_len);
        poly.pad16(aad_len);
    }
    if (ct_len > 0 && ct != nullptr) {
        poly.update(ct, ct_len);
        poly.pad16(ct_len);
    }

    uint8_t lens[16];
    store_le64(lens + 0, static_cast<uint64_t>(aad_len));
    store_le64(lens + 8, static_cast<uint64_t>(ct_len));
    poly.update(lens, 16);

    poly.finish(tag_out);
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) noexcept {
    Sha256Ctx ctx;
    ctx.update(data, len);
    ctx.finish(out);
}

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* data, size_t len,
                 uint8_t out[32]) noexcept {
    uint8_t k_block[64]{};
    if (key != nullptr && key_len > 0) {
        if (key_len > 64) {
            sha256(key, key_len, k_block);
        } else {
            std::memcpy(k_block, key, key_len);
        }
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k_block[i] ^ 0x36u);
        opad[i] = static_cast<uint8_t>(k_block[i] ^ 0x5cu);
    }

    uint8_t inner_hash[32];
    {
        Sha256Ctx inner;
        inner.update(ipad, 64);
        inner.update(data, len);
        inner.finish(inner_hash);
    }

    {
        Sha256Ctx outer;
        outer.update(opad, 64);
        outer.update(inner_hash, 32);
        outer.finish(out);
    }

    secure_zero(k_block, sizeof(k_block));
    secure_zero(ipad, sizeof(ipad));
    secure_zero(opad, sizeof(opad));
    secure_zero(inner_hash, sizeof(inner_hash));
}

void hkdf_derive_keys(const uint8_t* master_key, size_t key_len,
                      uint8_t out_wal_key[32],
                      uint8_t out_snap_key[32]) noexcept {
    // HKDF-Extract (RFC 5869 Section 2.2): PRK = HMAC-Hash(salt = 32 zero bytes, IKM)
    static constexpr uint8_t zero_salt[32]{};
    uint8_t prk[32];
    hmac_sha256(zero_salt, sizeof(zero_salt), master_key, key_len, prk);

    // HKDF-Expand (RFC 5869 Section 2.3), L = 32 -> single block T(1) = HMAC(PRK, info || 0x01)
    static constexpr uint8_t wal_info[] = {
        'z','e','p','h','y','r','-','w','a','l','-','v','1', 0x01u
    };
    static constexpr uint8_t snap_info[] = {
        'z','e','p','h','y','r','-','s','n','a','p','-','v','1', 0x01u
    };

    if (out_wal_key) {
        hmac_sha256(prk, sizeof(prk), wal_info, sizeof(wal_info), out_wal_key);
    }
    if (out_snap_key) {
        hmac_sha256(prk, sizeof(prk), snap_info, sizeof(snap_info), out_snap_key);
    }

    secure_zero(prk, sizeof(prk));
}

void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* pt, size_t pt_len,
                  uint8_t* ct_out, uint8_t tag_out[16]) noexcept {
    chacha20_xor(key, 1u, nonce, pt, pt_len, ct_out);
    compute_aead_tag(key, nonce, aad, aad_len, ct_out, pt_len, tag_out);
}

bool aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t* aad, size_t aad_len,
                  const uint8_t* ct, size_t ct_len,
                  const uint8_t tag[16], uint8_t* pt_out) noexcept {
    uint8_t expected_tag[16];
    compute_aead_tag(key, nonce, aad, aad_len, ct, ct_len, expected_tag);

    // Constant-time 16-byte tag comparison
    uint8_t diff = 0;
    for (size_t i = 0; i < 16; ++i) {
        diff |= static_cast<uint8_t>(expected_tag[i] ^ tag[i]);
    }
    secure_zero(expected_tag, sizeof(expected_tag));

    if (diff != 0) {
        return false;
    }

    chacha20_xor(key, 1u, nonce, ct, ct_len, pt_out);
    return true;
}

void random_bytes(uint8_t* out, size_t len) {
    if (!out || len == 0) {
        return;
    }

#if defined(_WIN32)
    using BCryptGenRandomFn = long(__stdcall*)(void*, unsigned char*, unsigned long, unsigned long);
    static BCryptGenRandomFn fn = []() -> BCryptGenRandomFn {
        HMODULE mod = LoadLibraryA("bcrypt.dll");
        if (!mod) {
            return nullptr;
        }
        return reinterpret_cast<BCryptGenRandomFn>(GetProcAddress(mod, "BCryptGenRandom"));
    }();
    if (fn != nullptr) {
        constexpr unsigned long kUseSystemPreferredRng = 0x00000002UL;
        size_t offset = 0;
        bool ok = true;
        while (offset < len) {
            const unsigned long chunk = static_cast<unsigned long>(
                std::min<size_t>(len - offset, 0x7FFFFFFFUL)
            );
            if (fn(nullptr, out + offset, chunk, kUseSystemPreferredRng) != 0) {
                ok = false;
                break;
            }
            offset += chunk;
        }
        if (ok) {
            return;
        }
    }
#elif !defined(__EMSCRIPTEN__)
    if (std::FILE* fp = std::fopen("/dev/urandom", "rb")) {
        const size_t got = std::fread(out, 1, len, fp);
        std::fclose(fp);
        if (got == len) {
            return;
        }
    }
#endif

    // Fallback: std::random_device
    std::random_device rd;
    size_t i = 0;
    while (i < len) {
        const uint32_t r = rd();
        const size_t take = std::min<size_t>(sizeof(r), len - i);
        std::memcpy(out + i, &r, take);
        i += take;
    }
}

bool self_test() noexcept {
    // 1. SHA-256 sanity check ("abc")
    {
        static constexpr uint8_t msg[3] = {'a', 'b', 'c'};
        static constexpr uint8_t expected[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        uint8_t digest[32];
        sha256(msg, sizeof(msg), digest);
        if (std::memcmp(digest, expected, 32) != 0) {
            return false;
        }
    }

    // 2. HKDF domain separation check
    {
        static constexpr uint8_t ikm[16] = {
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b
        };
        uint8_t wal_key[32]{};
        uint8_t snap_key[32]{};
        hkdf_derive_keys(ikm, sizeof(ikm), wal_key, snap_key);
        if (std::memcmp(wal_key, snap_key, 32) == 0) {
            return false;
        }
    }

    // 3. RFC 8439 Section 2.8.2 official ChaCha20-Poly1305 AEAD test vector
    static constexpr uint8_t key[32] = {
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
        0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f
    };
    static constexpr uint8_t nonce[12] = {
        0x07, 0x00, 0x00, 0x00,
        0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47
    };
    static constexpr uint8_t aad[12] = {
        0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3,
        0xc4, 0xc5, 0xc6, 0xc7
    };
    static constexpr char pt_str[] =
        "Ladies and Gentlemen of the class of '99: "
        "If I could offer you only one tip for the future, sunscreen would be it.";
    constexpr size_t pt_len = sizeof(pt_str) - 1; // 114 bytes
    static_assert(pt_len == 114, "RFC 8439 Section 2.8.2 plaintext must be 114 bytes");

    static constexpr uint8_t expected_ct[114] = {
        0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb,
        0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
        0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
        0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6,
        0x3d, 0xbe, 0xa4, 0x5e, 0x8c, 0xa9, 0x67, 0x12,
        0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
        0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29,
        0x05, 0xd6, 0xa5, 0xb6, 0x7e, 0xcd, 0x3b, 0x36,
        0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
        0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58,
        0xfa, 0xb3, 0x24, 0xe4, 0xfa, 0xd6, 0x75, 0x94,
        0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
        0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d,
        0xe5, 0x76, 0xd2, 0x65, 0x86, 0xce, 0xc6, 0x4b,
        0x61, 0x16
    };
    static constexpr uint8_t expected_tag[16] = {
        0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a,
        0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91
    };

    uint8_t ct[pt_len]{};
    uint8_t tag[16]{};
    aead_encrypt(key, nonce, aad, sizeof(aad),
                 reinterpret_cast<const uint8_t*>(pt_str), pt_len,
                 ct, tag);

    if (std::memcmp(ct, expected_ct, pt_len) != 0) {
        return false;
    }
    if (std::memcmp(tag, expected_tag, 16) != 0) {
        return false;
    }

    uint8_t pt_dec[pt_len]{};
    if (!aead_decrypt(key, nonce, aad, sizeof(aad), ct, pt_len, tag, pt_dec)) {
        return false;
    }
    if (std::memcmp(pt_dec, pt_str, pt_len) != 0) {
        return false;
    }

    // Tampered tag must fail authentication
    tag[15] ^= 0x01u;
    if (aead_decrypt(key, nonce, aad, sizeof(aad), ct, pt_len, tag, pt_dec)) {
        return false;
    }
    tag[15] ^= 0x01u;

    // Tampered ciphertext must fail authentication
    ct[0] ^= 0x80u;
    if (aead_decrypt(key, nonce, aad, sizeof(aad), ct, pt_len, tag, pt_dec)) {
        return false;
    }

    return true;
}

} // namespace zephyr::storage::crypto
