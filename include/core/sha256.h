/**
 * @file sha256.h
 * @brief SHA-256 (FIPS 180-4), portable C99, no heap. Used for salted PIN hashes.
 */

#ifndef INDUSTRIAL_HMI_SHA256_H
#define INDUSTRIAL_HMI_SHA256_H

#include <stdint.h>
#include <stddef.h>

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

#endif /* INDUSTRIAL_HMI_SHA256_H */
