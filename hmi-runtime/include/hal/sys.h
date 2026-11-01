/**
 * @file sys.h
 * @brief Operating-system facts the runtime needs: wall clock, random bytes, install
 *        directory and host load figures. Implemented by sys_win32.c and sys_posix.c.
 */

#ifndef INDUSTRIAL_HMI_HAL_SYS_H
#define INDUSTRIAL_HMI_HAL_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* "YYYY-MM-DDTHH:MM:SS.mmmZ" (UTC). On a target without a real-time clock this is the
 * time since 1970 as the C library reports it, which is only meaningful after an SNTP sync. */
void hal_utc_now(char *out, size_t cap);

/* Cryptographically secure random bytes from the OS (PIN salts). false = no source available. */
bool hal_random(void *out, size_t n);

/* Directory of the executable including the trailing separator ("" if unknown) and the
 * executable's size in bytes (0 if unknown). */
void     hal_exe_dir(char *out, size_t cap);
uint32_t hal_exe_size(void);

/* Overwrite memory in a way the optimiser may not remove (PINs). */
static inline void hal_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* Host load figures. A field is only valid when its *_ok flag is set; a platform that cannot
 * provide one leaves it false and the dashboard shows that tag as unavailable. */
typedef struct {
    bool     cpu_ok, ram_ok, proc_ok;
    int16_t  cpu_pct;          /* busy share since the previous call, 0..100 */
    int16_t  ram_pct;          /* memory in use, 0..100 */
    uint32_t proc_private_kb;  /* this process, private memory */
    uint32_t proc_resident_kb; /* this process, resident set */
} hal_host_stats_t;

void hal_host_stats_init(void);
void hal_host_stats_poll(hal_host_stats_t *out);

#endif /* INDUSTRIAL_HMI_HAL_SYS_H */
