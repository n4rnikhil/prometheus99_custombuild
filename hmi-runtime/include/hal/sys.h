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

/* The same clock as milliseconds since 1970-01-01T00:00:00Z. */
uint64_t hal_utc_ms(void);

/* Local time minus UTC, in seconds, now (time zone and daylight saving as the OS has them). */
int32_t  hal_local_offset_s(void);

/* Format a hal_utc_ms() value as "YYYY-MM-DDTHH:MM:SS.mmmZ" (portable, no C library time zone). */
void hal_utc_format(uint64_t utc_ms, char *out, size_t cap);

/* Cryptographically secure random bytes from the OS (PIN salts). false = no source available. */
bool hal_random(void *out, size_t n);

/* Directory of the executable including the trailing separator ("" if unknown) and the
 * executable's size in bytes (0 if unknown). */
void     hal_exe_dir(char *out, size_t cap);
uint32_t hal_exe_size(void);

/* The alarm tone (two-tone, 0.6 s, loops cleanly): 16-bit mono PCM at `rate` Hz, or the same as a
 * complete WAV file image. Return the samples / bytes written (0 = buffer too small). */
size_t hal_alarm_tone_pcm(int16_t *out, size_t max_samples, uint32_t rate);
size_t hal_alarm_tone_wav(uint8_t *out, size_t cap, uint32_t rate);

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
