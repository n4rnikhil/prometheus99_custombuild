/**
 * @file security.c
 * @brief Embedded Cybersecurity & Role-Based Access Control Implementation
 */

#include "../../include/core/security.h"
#include <string.h>

#define DEFAULT_LOGOUT_TIMEOUT_MS  (5 * 60 * 1000) /* 5 minutes of inactivity */

void hmi_security_init(hmi_security_ctx_t *sec)
{
    if (!sec) return;
    memset(sec, 0, sizeof(hmi_security_ctx_t));
    sec->current_role = ROLE_OPERATOR; /* Default role: Operator */
    sec->auto_logout_timeout_ms = DEFAULT_LOGOUT_TIMEOUT_MS;
    sec->high_contrast_theme = false;
}

void hmi_security_set_role(hmi_security_ctx_t *sec, hmi_role_t role, uint32_t now_ms)
{
    if (!sec) return;
    sec->current_role = role;
    sec->last_activity_ms = now_ms;
}

void hmi_security_register_activity(hmi_security_ctx_t *sec, uint32_t now_ms)
{
    if (sec) sec->last_activity_ms = now_ms;
}

void hmi_security_check_timeout(hmi_security_ctx_t *sec, uint32_t now_ms)
{
    if (!sec) return;
    /* Inactivity timeout: automatically revert to safe read-only Viewer role */
    if (sec->current_role > ROLE_VIEWER) {
        if ((now_ms - sec->last_activity_ms) >= sec->auto_logout_timeout_ms) {
            sec->current_role = ROLE_VIEWER;
        }
    }
}

bool hmi_security_can_acknowledge_alarm(const hmi_security_ctx_t *sec)
{
    if (!sec) return false;
    return (sec->current_role >= ROLE_OPERATOR);
}

bool hmi_security_can_operate_equipment(const hmi_security_ctx_t *sec)
{
    if (!sec) return false;
    return (sec->current_role >= ROLE_OPERATOR);
}

bool hmi_security_can_adjust_setpoint(const hmi_security_ctx_t *sec)
{
    if (!sec) return false;
    return (sec->current_role >= ROLE_MAINTENANCE);
}

bool hmi_security_can_configure_system(const hmi_security_ctx_t *sec)
{
    if (!sec) return false;
    return (sec->current_role >= ROLE_ENGINEER);
}

int16_t hmi_security_clamp_setpoint(int16_t requested, int16_t min_val, int16_t max_val)
{
    if (requested < min_val) return min_val;
    if (requested > max_val) return max_val;
    return requested;
}

bool hmi_security_validate_packet(const uint8_t *buffer, size_t len, size_t max_allowed)
{
    if (!buffer || len == 0 || len > max_allowed) {
        return false;
    }
    return true;
}
