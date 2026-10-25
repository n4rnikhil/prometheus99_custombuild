/**
 * @file security.h
 * @brief Role-based access control and setpoint clamping
 * @standards IEC 62443-4-2 CR 2.1 (authorisation enforcement)
 *
 * Every operator command goes through hmi_security_authorize() before it is
 * forwarded to the fieldbus thread. The emergency stop is never gated: any
 * role, including VIEWER, may trip it (IEC 60204-1 / ISO 13850).
 *
 * Authentication (IEC 62443-4-2 CR 1.1 / CR 1.11):
 *   - every role above VIEWER needs a 4..8 digit PIN
 *   - PINs are stored only as salted, iterated SHA-256 (never in clear)
 *   - comparison is constant-time
 *   - 3 wrong PINs lock PIN entry for 30 s (unsuccessful login attempts)
 *   - the vault (PIN records) serialises to a CRC32-C protected blob that the
 *     platform layer stores in security.vault
 *   - the HMI starts as VIEWER and drops back to VIEWER after inactivity
 */

#ifndef INDUSTRIAL_HMI_SECURITY_H
#define INDUSTRIAL_HMI_SECURITY_H

#include "../config.h"

#define HMI_PIN_MIN_LEN        4
#define HMI_PIN_MAX_LEN        8
#define HMI_PIN_MAX_ATTEMPTS   3
#define HMI_PIN_LOCKOUT_MS     30000u
#define HMI_PIN_HASH_ROUNDS    2000u
#define HMI_VAULT_MAGIC        0x56434553u   /* "SECV" little-endian */
#define HMI_VAULT_SIZE         144u

typedef struct {
    uint8_t salt[8];
    uint8_t hash[32];
} hmi_pin_record_t;

typedef enum {
    HMI_LOGIN_OK = 0,
    HMI_LOGIN_BAD_PIN,
    HMI_LOGIN_LOCKED,
    HMI_LOGIN_INVALID      /* malformed PIN or role */
} hmi_login_result_t;

typedef enum {
    HMI_CMD_NAVIGATE = 0,      /* any role */
    HMI_CMD_THEME,             /* any role */
    HMI_CMD_ROLE_SELECT,       /* any role (opens the PIN login) */
    HMI_CMD_ESTOP,             /* any role - never gated */
    HMI_CMD_ACK_ALARMS,        /* OPERATOR+ */
    HMI_CMD_SILENCE_HORN,      /* OPERATOR+ */
    HMI_CMD_PUMP_TOGGLE,       /* OPERATOR+ */
    HMI_CMD_VALVE_TOGGLE,      /* OPERATOR+ */
    HMI_CMD_ESTOP_RESET,       /* OPERATOR+ */
    HMI_CMD_WATCHDOG_RESET,    /* MAINTENANCE+ */
    HMI_CMD_INJECT_FAULT,      /* ENGINEER */
    HMI_CMD_OPERATE,           /* OPERATOR+: start / stop / mode / reset of plant equipment */
    HMI_CMD_SETPOINT,          /* OPERATOR+: process setpoints */
    HMI_CMD_TUNE,              /* MAINTENANCE+: tuning values (ramp times, limits) */
    HMI_CMD_RAW_WRITE,         /* ENGINEER: write any register of an unrecognised device */
    HMI_CMD_COUNT
} hmi_command_t;

typedef struct {
    hmi_role_t current_role;
    uint32_t   last_activity_ms;
    uint32_t   auto_logout_timeout_ms;
    bool       high_contrast_theme;
    /* Last denied command, for on-screen feedback */
    uint8_t    last_denied_cmd;        /* hmi_command_t */
    uint32_t   last_denied_ms;
    uint32_t   denied_count;
    bool       auto_logged_out;        /* set when the inactivity timeout dropped the role */
    /* Authentication */
    hmi_pin_record_t pins[HMI_ROLE_COUNT];   /* [0] (VIEWER) unused */
    bool       default_pins;           /* factory PINs still in use (shown as a warning) */
    uint8_t    failed_attempts;
    bool       locked;
    uint32_t   lockout_start_ms;
    uint32_t   login_failures_total;
} hmi_security_ctx_t;

/* --- Authentication --- */
bool hmi_security_role_needs_pin(hmi_role_t role);
/* Factory PINs: Operator 1111, Maintenance 2222, Engineer 3333 (sets default_pins). */
void hmi_security_load_default_pins(hmi_security_ctx_t *sec);
/* Store a new PIN for role with the given salt. false if role/PIN invalid. Clears default_pins. */
bool hmi_security_set_pin(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, const uint8_t salt[8]);
/* Try to switch to role with pin. VIEWER never needs a PIN. On success the role is set. */
hmi_login_result_t hmi_security_login(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, uint32_t now_ms);
uint32_t hmi_security_lockout_remaining_ms(hmi_security_ctx_t *sec, uint32_t now_ms);
/* Vault blob (HMI_VAULT_SIZE bytes): magic, version, 3 PIN records, CRC32-C. */
size_t hmi_security_vault_export(const hmi_security_ctx_t *sec, uint8_t *out, size_t cap);
bool   hmi_security_vault_import(hmi_security_ctx_t *sec, const uint8_t *in, size_t len);

void hmi_security_init(hmi_security_ctx_t *sec);
void hmi_security_set_role(hmi_security_ctx_t *sec, hmi_role_t role, uint32_t now_ms);
void hmi_security_register_activity(hmi_security_ctx_t *sec, uint32_t now_ms);
/* Returns true if this call performed an auto-logout (role dropped to VIEWER). */
bool hmi_security_check_timeout(hmi_security_ctx_t *sec, uint32_t now_ms);

/* Minimum role per command */
hmi_role_t hmi_security_required_role(hmi_command_t cmd);
/* true = allowed. On denial records last_denied_* and increments denied_count. */
bool hmi_security_authorize(hmi_security_ctx_t *sec, hmi_command_t cmd, uint32_t now_ms);

const char* hmi_security_role_name(hmi_role_t role);   /* "VIEWER","OPERATOR","MAINT","ENGINEER" */
const char* hmi_security_command_name(hmi_command_t cmd);

/* Legacy gates (wrappers over hmi_security_required_role) */
bool hmi_security_can_acknowledge_alarm(const hmi_security_ctx_t *sec);
bool hmi_security_can_operate_equipment(const hmi_security_ctx_t *sec);
bool hmi_security_can_adjust_setpoint(const hmi_security_ctx_t *sec);
bool hmi_security_can_configure_system(const hmi_security_ctx_t *sec);

int16_t hmi_security_clamp_setpoint(int16_t requested, int16_t min_val, int16_t max_val);
bool hmi_security_validate_packet(const uint8_t *buffer, size_t len, size_t max_allowed);

#endif /* INDUSTRIAL_HMI_SECURITY_H */
