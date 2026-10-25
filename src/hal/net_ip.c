/**
 * @file net_ip.c
 * @brief Portable IPv4 text helpers (no OS headers)
 */

#include "../../include/hal/net.h"
#include <stdio.h>
#include <string.h>

bool hal_ip_parse(const char *text, uint32_t *ip)
{
    if (!text || !ip) return false;
    if (strcmp(text, "localhost") == 0) { *ip = 0x7F000001u; return true; }
    uint32_t v = 0;
    const char *p = text;
    for (int part = 0; part < 4; part++) {
        if (*p < '0' || *p > '9') return false;
        uint32_t octet = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            octet = octet * 10u + (uint32_t)(*p - '0');
            if (++digits > 3 || octet > 255u) return false;
            p++;
        }
        v = (v << 8) | octet;
        if (part < 3) {
            if (*p != '.') return false;
            p++;
        }
    }
    if (*p != '\0') return false;
    *ip = v;
    return true;
}

void hal_ip_format(uint32_t ip, char out[16])
{
    snprintf(out, 16, "%u.%u.%u.%u", (unsigned)((ip >> 24) & 255u), (unsigned)((ip >> 16) & 255u),
             (unsigned)((ip >> 8) & 255u), (unsigned)(ip & 255u));
}
