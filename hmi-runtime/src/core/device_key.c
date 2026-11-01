/**
 * @file device_key.c
 * @brief Per-installation secret key (see device_key.h). Portable: file and random via the HAL.
 */

#include <stdio.h>
#include <string.h>

#include "core/device_key.h"
#include "core/crc32.h"
#include "core/sha256.h"
#include "hal/fs.h"
#include "hal/sys.h"

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

hmi_key_status_t hmi_device_key_load(const char *path, bool create, uint8_t key[HMI_DEVICE_KEY_LEN])
{
    uint8_t buf[HMI_DEVICE_KEY_FILE_LEN + 1];
    if (!path || !key) return HMI_KEY_MISSING;

    hal_file_t *f = hal_file_open(path, HAL_FILE_READ);
    if (f) {
        int got = hal_file_read(f, buf, sizeof(buf));
        hal_file_close(f);
        hmi_key_status_t st = HMI_KEY_DAMAGED;
        if (got == (int)HMI_DEVICE_KEY_FILE_LEN && get_u32(buf) == HMI_DEVICE_KEY_MAGIC && buf[4] == 1 &&
            get_u32(buf + 40) == hmi_crc32c(buf, 40)) {
            memcpy(key, buf + 8, HMI_DEVICE_KEY_LEN);
            st = HMI_KEY_LOADED;
        }
        wipe(buf, sizeof(buf));
        return st;                       /* a damaged key is reported, never silently replaced */
    }
    if (!create) return HMI_KEY_MISSING;

    if (!hal_random(key, HMI_DEVICE_KEY_LEN)) return HMI_KEY_NO_RANDOM;
    memset(buf, 0, sizeof(buf));
    put_u32(buf, HMI_DEVICE_KEY_MAGIC);
    buf[4] = 1;
    memcpy(buf + 8, key, HMI_DEVICE_KEY_LEN);
    put_u32(buf + 40, hmi_crc32c(buf, 40));

    char tmp[300];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) { wipe(buf, sizeof(buf)); return HMI_KEY_WRITE_FAILED; }
    f = hal_file_open(tmp, HAL_FILE_WRITE_NEW);
    bool ok = f != NULL;
    if (ok) hal_file_restrict(tmp);      /* before the secret is written */
    ok = ok && hal_file_write(f, buf, HMI_DEVICE_KEY_FILE_LEN) && hal_file_sync(f);
    hal_file_close(f);
    wipe(buf, sizeof(buf));
    if (!ok || !hal_file_replace(tmp, path)) {
        hal_file_remove(tmp);
        return HMI_KEY_WRITE_FAILED;
    }
    return HMI_KEY_CREATED;
}

void hmi_device_key_id(const uint8_t key[HMI_DEVICE_KEY_LEN], char out[9])
{
    uint8_t h[32];
    hmi_sha256(key, HMI_DEVICE_KEY_LEN, h);
    snprintf(out, 9, "%02X%02X%02X%02X", h[0], h[1], h[2], h[3]);
}

const char *hmi_device_key_status_name(hmi_key_status_t st)
{
    switch (st) {
        case HMI_KEY_LOADED:       return "loaded";
        case HMI_KEY_CREATED:      return "created";
        case HMI_KEY_MISSING:      return "missing";
        case HMI_KEY_DAMAGED:      return "damaged";
        case HMI_KEY_NO_RANDOM:    return "no secure random source";
        case HMI_KEY_WRITE_FAILED: return "could not be written";
        default:                   return "unknown";
    }
}
