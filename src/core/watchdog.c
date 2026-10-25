/**
 * @file watchdog.c
 * @brief Heartbeat watchdog with latched failover (wrap-safe)
 */

#include "../../include/core/watchdog.h"
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

static bool heartbeat_fresh(uint32_t now_ms, uint32_t ping_ms)
{
    /* Unsigned subtraction is wrap-safe. A ping slightly in the future (another
     * thread sampled the clock after us) shows up as a huge age; treat ages above
     * half the range as "just now". */
    uint32_t age = now_ms - ping_ms;
    if (age > 0x80000000u) return true;
    return age <= HMI_WATCHDOG_MAX_AGE_MS;
}

bool hmi_watchdog_evaluate(hmi_watchdog_t *wd, uint32_t now_ms)
{
    if (!wd) return false;

    bool was_healthy = wd->system_healthy;
    wd->fieldbus_alive = heartbeat_fresh(now_ms, wd->last_fieldbus_ping_ms);
    wd->ui_alive       = heartbeat_fresh(now_ms, wd->last_ui_ping_ms);
    wd->system_healthy = (wd->fieldbus_alive && wd->ui_alive);

    if (wd->system_healthy) {
        wd->total_pets++;
        return false;
    }
    wd->failover_active = true;
    if (was_healthy) {
        wd->fault_count++;
        return true;
    }
    return false;
}

bool hmi_watchdog_reset_failover(hmi_watchdog_t *wd)
{
    if (!wd || !wd->system_healthy) return false;
    wd->failover_active = false;
    return true;
}
