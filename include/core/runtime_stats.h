/**
 * @file runtime_stats.h
 * @brief Measured fieldbus-loop timing, published to the UI each iteration
 */

#ifndef INDUSTRIAL_HMI_RUNTIME_STATS_H
#define INDUSTRIAL_HMI_RUNTIME_STATS_H

#include "../config.h"

typedef struct {
    uint32_t loop_count;        /* iterations since start */
    uint32_t loop_hz;           /* iterations in the last complete 1 s window */
    uint32_t period_avg_us;     /* over the last complete 1 s window */
    uint32_t period_min_us;
    uint32_t period_max_us;     /* worst-case jitter indicator */
    uint32_t late_count;        /* cumulative iterations with period > 2 x nominal */
    uint32_t pacer_overruns;    /* cumulative schedule re-bases */
    uint32_t publish_ms;        /* uptime ms at publish */
    uint32_t snapshot_crc;      /* CRC32-C over the published tag table */
    bool     high_res_timer;    /* waitable high-resolution timer in use */
} hmi_fieldbus_stats_t;

#endif /* INDUSTRIAL_HMI_RUNTIME_STATS_H */
