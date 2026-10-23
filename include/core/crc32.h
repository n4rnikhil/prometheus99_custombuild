/**
 * @file crc32.h
 * @brief Industrial CRC32-C (Castagnoli Polynomial 0x82F63B78)
 * @standard RFC 3720 / iSCSI / IEC 62443 Data Integrity Standard
 *
 * Micro-footprint 16-entry nibble lookup table (only 64 bytes of ROM).
 */

#ifndef INDUSTRIAL_HMI_CRC32_H
#define INDUSTRIAL_HMI_CRC32_H

#include <stdint.h>
#include <stddef.h>

uint32_t hmi_crc32c(const uint8_t *data, size_t len);

#endif /* INDUSTRIAL_HMI_CRC32_H */
