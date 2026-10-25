/**
 * @file crc32.c
 * @brief Industrial CRC32-C (Castagnoli Polynomial 0x82F63B78)
 * @standards RFC 3720 / iSCSI / IEC 62443 Data Integrity Standard
 */

#include "../../include/core/crc32.h"

static const uint32_t s_crc32c_nibble_table[16] = {
    0x00000000, 0x105EC76F, 0x20BD8EDE, 0x30E349B1,
    0x417B1DBC, 0x5125DAD3, 0x61C69362, 0x7198540D,
    0x82F63B78, 0x92A8FC17, 0xA24BB5A6, 0xB21572C9,
    0xC38D26C4, 0xD3D3E1AB, 0xE330A81A, 0xF36E6F75
};

uint32_t hmi_crc32c(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return 0;

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        crc = (crc >> 4) ^ s_crc32c_nibble_table[(crc ^ (byte & 0x0Fu)) & 0x0Fu];
        crc = (crc >> 4) ^ s_crc32c_nibble_table[(crc ^ (byte >> 4)) & 0x0Fu];
    }
    return crc ^ 0xFFFFFFFFu;
}
