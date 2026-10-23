/**
 * @file security.h
 * @brief Embedded Cybersecurity & Role-Based Access Control (RBAC)
 * @standard IEC 62443-4-2 Technical Security Requirements for IACS Components
 */

#ifndef INDUSTRIAL_HMI_SECURITY_H
#define INDUSTRIAL_HMI_SECURITY_H

#include "../config.h"

typedef struct {
    hmi_role_t current_role;
    uint32_t   last_activity_ms;
    uint32_t   auto_logout_timeout_ms;
    bool       high_contrast_theme;
} hmi_security_ctx_t;

void hmi_security_init(hmi_security_ctx_t *sec);
void hmi_security_set_role(hmi_security_ctx_t *sec, hmi_role_t role, uint32_t now_ms);
void hmi_security_register_activity(hmi_security_ctx_t *sec, uint32_t now_ms);
void hmi_security_check_timeout(hmi_security_ctx_t *sec, uint32_t now_ms);

/* Authorization Gates */
bool hmi_security_can_acknowledge_alarm(const hmi_security_ctx_t *sec);
bool hmi_security_can_operate_equipment(const hmi_security_ctx_t *sec);
bool hmi_security_can_adjust_setpoint(const hmi_security_ctx_t *sec);
bool hmi_security_can_configure_system(const hmi_security_ctx_t *sec);

/* Setpoint Safety Clamping */
int16_t hmi_security_clamp_setpoint(int16_t requested, int16_t min_val, int16_t max_val);

/* Packet Boundary Validator (Buffer Overflow Defense) */
bool hmi_security_validate_packet(const uint8_t *buffer, size_t len, size_t max_allowed);

#endif /* INDUSTRIAL_HMI_SECURITY_H */
