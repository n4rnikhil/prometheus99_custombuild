/**
 * @file watchdog.h
 * @brief Heartbeat watchdog used for cross-supervision of the two loops
 * @standard IEC 61508-style watchdog with latched failover
 *
 * Each loop publishes a heartbeat timestamp. The UI thread runs one instance
 * that supervises the fieldbus heartbeat; the fieldbus thread runs another that
 * supervises the UI heartbeat. A loop is never judged by its own watchdog.
 *
 * All timestamp comparisons are wrap-safe (uint32 ms, 49.7-day wrap).
 */

#ifndef INDUSTRIAL_HMI_WATCHDOG_H
#define INDUSTRIAL_HMI_WATCHDOG_H

#include "../config.h"

typedef struct {
    uint32_t last_fieldbus_ping_ms;
    uint32_t last_ui_ping_ms;
    uint32_t total_pets;        /* healthy evaluations */
    uint32_t fault_count;       /* healthy -> faulted transitions */
    bool     fieldbus_alive;
    bool     ui_alive;
    bool     system_healthy;
    bool     failover_active;   /* latched on a fault; cleared only by hmi_watchdog_reset_failover */
} hmi_watchdog_t;

void hmi_watchdog_init(hmi_watchdog_t *wd, uint32_t now_ms);
void hmi_watchdog_ping_fieldbus(hmi_watchdog_t *wd, uint32_t now_ms);
void hmi_watchdog_ping_ui(hmi_watchdog_t *wd, uint32_t now_ms);

/* Re-evaluate liveness. Returns true when the transition healthy->faulted happened
 * on THIS call (so the caller can log it once). */
bool hmi_watchdog_evaluate(hmi_watchdog_t *wd, uint32_t now_ms);

/* Clear the failover latch. Only succeeds while the system is healthy. */
bool hmi_watchdog_reset_failover(hmi_watchdog_t *wd);

#endif /* INDUSTRIAL_HMI_WATCHDOG_H */
