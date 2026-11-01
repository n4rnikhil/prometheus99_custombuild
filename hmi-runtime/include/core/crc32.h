/**
 * @file crc32.h
 * @brief Industrial CRC32-C (Castagnoli Polynomial 0x82F63B78)
 * @standards CRC32-C (Castagnoli) as in RFC 3720. Detects accidental damage only; it is not a
 *            security measure (signatures use HMAC-SHA256, see sha256.h).
 */

#ifndef INDUSTRIAL_HMI_CRC32_H
#define INDUSTRIAL_HMI_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t hmi_crc32c(const uint8_t *data, size_t len);

#endif /* INDUSTRIAL_HMI_CRC32_H */
