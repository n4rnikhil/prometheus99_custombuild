/**
 * @file sha256.h
 * @brief SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104 / FIPS 198-1), portable C99, no heap.
 *        Used for PIN hashes, the keyed audit-log chain and the credential vault MAC.
 */

#ifndef INDUSTRIAL_HMI_SHA256_H
#define INDUSTRIAL_HMI_SHA256_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  buf[64];
    uint32_t buf_len;
} hmi_sha256_t;

void hmi_sha256_init(hmi_sha256_t *c);
void hmi_sha256_update(hmi_sha256_t *c, const void *data, size_t len);
void hmi_sha256_final(hmi_sha256_t *c, uint8_t out[32]);
void hmi_sha256(const void *data, size_t len, uint8_t out[32]);

/* HMAC-SHA256, streaming. Keys longer than 64 bytes are hashed first (RFC 2104). */
typedef struct {
    hmi_sha256_t inner;
    hmi_sha256_t outer;
} hmi_hmac_sha256_t;

void hmi_hmac_sha256_init(hmi_hmac_sha256_t *c, const void *key, size_t key_len);
void hmi_hmac_sha256_update(hmi_hmac_sha256_t *c, const void *data, size_t len);
void hmi_hmac_sha256_final(hmi_hmac_sha256_t *c, uint8_t out[32]);
void hmi_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[32]);

/* HKDF-SHA256 (RFC 5869): extract with salt (may be NULL), then expand to out_len bytes
 * (at most 255 * 32). Used to derive one key per purpose from the device key. */
void hmi_hkdf_sha256(const void *salt, size_t salt_len, const void *ikm, size_t ikm_len,
                     const void *info, size_t info_len, uint8_t *out, size_t out_len);

/* Constant-time comparison of two n-byte buffers: true if equal. */
bool hmi_ct_equal(const void *a, const void *b, size_t n);

#endif /* INDUSTRIAL_HMI_SHA256_H */
