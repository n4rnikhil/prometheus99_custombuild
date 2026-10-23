/**
 * @file watchdog.h
 * @brief Independent Dual-Loop Watchdog Supervisor
 * @standard IEC 61508 SIL-ready Functional Safety
 */

#ifndef INDUSTRIAL_HMI_WATCHDOG_H
#define INDUSTRIAL_HMI_WATCHDOG_H

#include "../config.h"

typedef struct {
    uint32_t last_fieldbus_ping_ms;
    uint32_t last_ui_ping_ms;
    uint32_t total_pets;
    uint32_t fault_count;
    bool     fieldbus_alive;
    bool     ui_alive;
    bool     system_healthy;
    bool     failover_active;
} hmi_watchdog_t;

void hmi_watchdog_init(hmi_watchdog_t *wd, uint32_t now_ms);
void hmi_watchdog_ping_fieldbus(hmi_watchdog_t *wd, uint32_t now_ms);
void hmi_watchdog_ping_ui(hmi_watchdog_t *wd, uint32_t now_ms);

/* Periodic verification tick (called by independent supervisor or timer) */
bool hmi_watchdog_evaluate(hmi_watchdog_t *wd, uint32_t now_ms);

/* Simulate fault for safety testing */
void hmi_watchdog_inject_fault(hmi_watchdog_t *wd);

#endif /* INDUSTRIAL_HMI_WATCHDOG_H */
