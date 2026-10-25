/**
 * @file timer.h
 * @brief Monotonic time base and a drift-free periodic pacer
 *
 * hmi_timer_get_ms() wraps every 49.7 days; always compare with
 * (uint32_t)(now - then) >= period, never with now >= then + period.
 */

#ifndef INDUSTRIAL_HMI_TIMER_H
#define INDUSTRIAL_HMI_TIMER_H

#include <stdint.h>
#include <stdbool.h>

void     hmi_timer_init(void);       /* timeBeginPeriod(1) + QPC base */
void     hmi_timer_shutdown(void);   /* timeEndPeriod(1) */
uint32_t hmi_timer_get_ms(void);
uint64_t hmi_timer_get_us(void);     /* microseconds since init */
void     hmi_timer_sleep_ms(uint32_t ms);

/* Periodic pacer: absolute deadlines (next += period) so the average rate does not
 * drift. Uses a high-resolution waitable timer when available
 * (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803+), else Sleep(1).
 * If the caller falls more than 10 periods behind, the schedule is re-based to
 * now (counted as an overrun) instead of bursting to catch up. */
typedef struct {
    void    *handle;            /* HANDLE of the waitable timer or NULL */
    uint64_t next_deadline_us;
    uint32_t period_us;
    bool     high_res;
    uint32_t overruns;          /* schedule re-bases */
} hmi_pacer_t;

bool hmi_pacer_init(hmi_pacer_t *p, uint32_t period_us);
void hmi_pacer_wait(hmi_pacer_t *p);
void hmi_pacer_close(hmi_pacer_t *p);

#endif /* INDUSTRIAL_HMI_TIMER_H */
