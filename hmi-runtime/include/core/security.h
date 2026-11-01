/**
 * @file security.h
 * @brief Role-based access control and setpoint clamping
 * @standards IEC 62443-4-2 CR 2.1 (authorisation enforcement)
 *
 * Every operator command goes through hmi_security_authorize() before it is
 * forwarded to the fieldbus thread. The HMI "STOP ALL" command is never gated: any role,
 * including VIEWER, may send it. It is a normal (category 0/1/2) stop request over the network,
 * NOT an emergency stop in the sense of ISO 13850 / IEC 60204-1 10.7: the emergency stop of a
 * machine must be a hardwired device that does not depend on this software or on Modbus.
 *
 * Authentication (IEC 62443-4-2 CR 1.1, CR 1.5, CR 1.7, CR 1.11):
 *   - every role above VIEWER needs a PIN; site PINs have 6..8 digits and may not be trivial
 *     (all one digit, or a straight run such as 123456)
 *   - PINs are stored only as salted, iterated HMAC-SHA256 keyed with the device key, so a
 *     copied vault cannot be brute-forced without the key file as well
 *   - comparison is constant-time
 *   - 3 wrong PINs lock PIN entry for 30 s (unsuccessful login attempts)
 *   - commissioning gate (CR 1.5, change of default authenticators): a role whose PIN is still
 *     the factory PIN cannot sign in, unless the bench override allow_factory_pins is set
 *   - the vault (PIN records) serialises to a blob signed with HMAC-SHA256 under the device key
 *   - the HMI starts as VIEWER and drops back to VIEWER after inactivity
 */

#ifndef INDUSTRIAL_HMI_SECURITY_H
#define INDUSTRIAL_HMI_SECURITY_H

#include "config.h"

#define HMI_PIN_MIN_LEN        4      /* shortest PIN the keypad accepts (factory PINs) */
#define HMI_PIN_SITE_MIN_LEN   6      /* shortest PIN that can be set for a site */
#define HMI_PIN_MAX_LEN        8
#define HMI_PIN_MAX_ATTEMPTS   3
#define HMI_PIN_LOCKOUT_MS     30000u
#define HMI_PIN_HASH_ROUNDS    2000u
#define HMI_VAULT_MAGIC        0x56434553u   /* "SECV" little-endian */
#define HMI_VAULT_VERSION      2u
#define HMI_VAULT_SIZE         172u          /* v2: 140 bytes of records + 32-byte HMAC */
#define HMI_VAULT_V1_SIZE      144u          /* v1: CRC only (older builds) - refused */

typedef struct {
    uint8_t salt[8];
    uint8_t hash[32];
} hmi_pin_record_t;

typedef enum {
    HMI_LOGIN_OK = 0,
    HMI_LOGIN_BAD_PIN,
    HMI_LOGIN_LOCKED,
    HMI_LOGIN_INVALID,     /* malformed PIN or role */
    HMI_LOGIN_FACTORY_PIN  /* the role still has its factory PIN: commissioning not done */
} hmi_login_result_t;

typedef enum {
    HMI_VAULT_OK = 0,
    HMI_VAULT_DAMAGED,     /* wrong size, magic or layout */
    HMI_VAULT_LEGACY,      /* v1 vault from an older build: unsigned, refused */
    HMI_VAULT_AUTH_FAILED  /* signature does not match: edited, or another device key */
} hmi_vault_result_t;

typedef enum {
    HMI_PIN_SET_OK = 0,
    HMI_PIN_SET_BAD_ROLE,
    HMI_PIN_SET_FORMAT,    /* not 6..8 digits */
    HMI_PIN_SET_TRIVIAL    /* one repeated digit, or a straight ascending / descending run */
} hmi_pin_set_result_t;

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
    bool       factory[HMI_ROLE_COUNT];/* this role still has its factory PIN */
    bool       default_pins;           /* any role still has its factory PIN (shown as a warning) */
    bool       allow_factory_pins;     /* bench override of the commissioning gate (--allow-factory-pins) */
    uint8_t    pin_key[32];            /* HKDF(credentials key, "pin-hash"): keys the PIN hashes */
    uint8_t    vault_key[32];          /* HKDF(credentials key, "vault-mac"): signs the vault */
    uint8_t    failed_attempts;
    bool       locked;
    uint32_t   lockout_start_ms;
    uint32_t   login_failures_total;
} hmi_security_ctx_t;

/* --- Authentication --- */
bool hmi_security_role_needs_pin(hmi_role_t role);
/* Credentials key (32 bytes, hmi_device_key_derive(..., CREDENTIALS)). Two keys are derived from
 * it: one for the PIN hashes, one for the vault signature. Set it before loading the vault or
 * setting PINs; factory PINs are re-hashed with it. NULL = all-zero key. */
void hmi_security_set_device_key(hmi_security_ctx_t *sec, const uint8_t *key32);
/* Factory PINs: Operator 1111, Maintenance 2222, Engineer 3333 (marks every role factory). */
void hmi_security_load_default_pins(hmi_security_ctx_t *sec);
/* Store a new site PIN for role with the given salt. Clears that role's factory flag. */
hmi_pin_set_result_t hmi_security_set_pin(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, const uint8_t salt[8]);
const char *hmi_security_pin_set_message(hmi_pin_set_result_t r);
/* Try to switch to role with pin. VIEWER never needs a PIN. On success the role is set. */
hmi_login_result_t hmi_security_login(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, uint32_t now_ms);
uint32_t hmi_security_lockout_remaining_ms(hmi_security_ctx_t *sec, uint32_t now_ms);
/* Vault blob (HMI_VAULT_SIZE bytes): magic, version, 3 PIN records, HMAC-SHA256 (device key). */
size_t             hmi_security_vault_export(const hmi_security_ctx_t *sec, uint8_t *out, size_t cap);
hmi_vault_result_t hmi_security_vault_import(hmi_security_ctx_t *sec, const uint8_t *in, size_t len);

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
