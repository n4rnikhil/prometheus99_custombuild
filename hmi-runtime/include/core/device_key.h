/**
 * @file device_key.h
 * @brief Per-installation secret key for the keyed audit-log chain and the credential vault
 * @standards IEC 62443-4-2 CR 2.8 / CR 3.9 (protection of audit information), CR 1.5
 *
 * 32 random bytes from the OS generator, created on the first start and stored in a small
 * file next to the executable (default "hmi.key"), limited to the runtime's own account.
 * Whoever can read this file can forge the audit log and the vault; whoever cannot, cannot.
 * Keep it out of backups that leave the site, and out of version control.
 *
 * File layout (44 bytes):
 *   0  u32 magic "HKEY" (little-endian)   4  u8 version = 1   5..7 reserved
 *   8  32 key bytes                       40 u32 CRC32-C of bytes 0..39
 */

#ifndef INDUSTRIAL_HMI_DEVICE_KEY_H
#define INDUSTRIAL_HMI_DEVICE_KEY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HMI_DEVICE_KEY_LEN      32u
#define HMI_DEVICE_KEY_FILE_LEN 44u
#define HMI_DEVICE_KEY_MAGIC    0x59454B48u   /* "HKEY" little-endian */

typedef enum {
    HMI_KEY_LOADED = 0,     /* existing key read and checked */
    HMI_KEY_CREATED,        /* no key file: a new one was generated and saved */
    HMI_KEY_MISSING,        /* no key file and create == false */
    HMI_KEY_DAMAGED,        /* file present but not a valid key: never overwritten */
    HMI_KEY_NO_RANDOM,      /* the OS has no secure random source */
    HMI_KEY_WRITE_FAILED
} hmi_key_status_t;

hmi_key_status_t hmi_device_key_load(const char *path, bool create, uint8_t key[HMI_DEVICE_KEY_LEN]);

/* 8 hex digits that name a key without revealing it (first 4 bytes of SHA-256 over it). */
void hmi_device_key_id(const uint8_t key[HMI_DEVICE_KEY_LEN], char out[9]);

const char *hmi_device_key_status_name(hmi_key_status_t st);

#endif /* INDUSTRIAL_HMI_DEVICE_KEY_H */
