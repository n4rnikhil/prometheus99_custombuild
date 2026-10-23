/**
 * @file watchdog.c
 * @brief Independent Dual-Loop Watchdog Supervisor Implementation
 */

#include "../../include/core/watchdog.h"
#include <stdio.h>
#include <string.h>

void hmi_watchdog_init(hmi_watchdog_t *wd, uint32_t now_ms)
{
    if (!wd) return;
    memset(wd, 0, sizeof(hmi_watchdog_t));
    wd->last_fieldbus_ping_ms = now_ms;
    wd->last_ui_ping_ms = now_ms;
    wd->fieldbus_alive = true;
    wd->ui_alive = true;
    wd->system_healthy = true;
    wd->failover_active = false;
}

void hmi_watchdog_ping_fieldbus(hmi_watchdog_t *wd, uint32_t now_ms)
{
    if (wd) wd->last_fieldbus_ping_ms = now_ms;
}

void hmi_watchdog_ping_ui(hmi_watchdog_t *wd, uint32_t now_ms)
{
    if (wd) wd->last_ui_ping_ms = now_ms;
}

bool hmi_watchdog_evaluate(hmi_watchdog_t *wd, uint32_t now_ms)
{
    if (!wd) return false;

    /* Verify heartbeat freshness */
    wd->fieldbus_alive = ((now_ms - wd->last_fieldbus_ping_ms) <= HMI_WATCHDOG_MAX_AGE_MS);
    wd->ui_alive       = ((now_ms - wd->last_ui_ping_ms) <= HMI_WATCHDOG_MAX_AGE_MS);

    /* DUAL-LOOP AND RULE: Both loops must be alive! */
    wd->system_healthy = (wd->fieldbus_alive && wd->ui_alive);

    if (wd->system_healthy) {
        wd->total_pets++;
    } else {
        wd->fault_count++;
        wd->failover_active = true;
    }

    return wd->system_healthy;
}

void hmi_watchdog_inject_fault(hmi_watchdog_t *wd)
{
    if (!wd) return;
    wd->last_fieldbus_ping_ms = 0;
    wd->system_healthy = false;
    wd->failover_active = true;
    wd->fault_count++;
}
