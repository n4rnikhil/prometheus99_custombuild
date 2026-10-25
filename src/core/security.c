/**
 * @file security.c
 * @brief Role-based access control (IEC 62443-4-2 CR 2.1) and input validation
 */

#include "../../include/core/security.h"
#include "../../include/core/sha256.h"
#include "../../include/core/crc32.h"
#include <string.h>

#define DEFAULT_LOGOUT_TIMEOUT_MS  (5u * 60u * 1000u)  /* 5 minutes of inactivity */

static const hmi_role_t s_required_role[HMI_CMD_COUNT] = {
    [HMI_CMD_NAVIGATE]       = ROLE_VIEWER,
    [HMI_CMD_THEME]          = ROLE_VIEWER,
    [HMI_CMD_ROLE_SELECT]    = ROLE_VIEWER,
    [HMI_CMD_ESTOP]          = ROLE_VIEWER,      /* never gated */
    [HMI_CMD_ACK_ALARMS]     = ROLE_OPERATOR,
    [HMI_CMD_SILENCE_HORN]   = ROLE_OPERATOR,
    [HMI_CMD_PUMP_TOGGLE]    = ROLE_OPERATOR,
    [HMI_CMD_VALVE_TOGGLE]   = ROLE_OPERATOR,
    [HMI_CMD_ESTOP_RESET]    = ROLE_OPERATOR,
    [HMI_CMD_WATCHDOG_RESET] = ROLE_MAINTENANCE,
    [HMI_CMD_INJECT_FAULT]   = ROLE_ENGINEER,
    [HMI_CMD_OPERATE]        = ROLE_OPERATOR,
    [HMI_CMD_SETPOINT]       = ROLE_OPERATOR,
    [HMI_CMD_TUNE]           = ROLE_MAINTENANCE,
    [HMI_CMD_RAW_WRITE]      = ROLE_ENGINEER,
};

static const char * const s_cmd_names[HMI_CMD_COUNT] = {
    [HMI_CMD_NAVIGATE]       = "Navigate",
    [HMI_CMD_THEME]          = "Theme",
    [HMI_CMD_ROLE_SELECT]    = "Role select",
    [HMI_CMD_ESTOP]          = "E-stop",
    [HMI_CMD_ACK_ALARMS]     = "Acknowledge",
    [HMI_CMD_SILENCE_HORN]   = "Silence horn",
    [HMI_CMD_PUMP_TOGGLE]    = "Pump start/stop",
    [HMI_CMD_VALVE_TOGGLE]   = "Valve move",
    [HMI_CMD_ESTOP_RESET]    = "E-stop reset",
    [HMI_CMD_WATCHDOG_RESET] = "Watchdog reset",
    [HMI_CMD_INJECT_FAULT]   = "Fault injection",
    [HMI_CMD_OPERATE]        = "Operate equipment",
    [HMI_CMD_SETPOINT]       = "Change setpoint",
    [HMI_CMD_TUNE]           = "Change tuning",
    [HMI_CMD_RAW_WRITE]      = "Write register",
};

void hmi_security_init(hmi_security_ctx_t *sec)
{
    if (!sec) return;
    memset(sec, 0, sizeof(hmi_security_ctx_t));
    sec->current_role = ROLE_VIEWER;          /* least privilege until a PIN login */
    sec->auto_logout_timeout_ms = DEFAULT_LOGOUT_TIMEOUT_MS;
    sec->high_contrast_theme = false;
    hmi_security_load_default_pins(sec);     /* replaced by the vault when one exists */
}

void hmi_security_set_role(hmi_security_ctx_t *sec, hmi_role_t role, uint32_t now_ms)
{
    if (!sec) return;
    if ((int)role < (int)ROLE_VIEWER || (int)role > (int)ROLE_ENGINEER) role = ROLE_VIEWER;
    sec->current_role = role;
    sec->last_activity_ms = now_ms;
    sec->auto_logged_out = false;
}

void hmi_security_register_activity(hmi_security_ctx_t *sec, uint32_t now_ms)
{
    if (sec) sec->last_activity_ms = now_ms;
}

bool hmi_security_check_timeout(hmi_security_ctx_t *sec, uint32_t now_ms)
{
    if (!sec) return false;
    if (sec->current_role > ROLE_VIEWER &&
        (uint32_t)(now_ms - sec->last_activity_ms) >= sec->auto_logout_timeout_ms) {
        sec->current_role = ROLE_VIEWER;
        sec->auto_logged_out = true;
        return true;
    }
    return false;
}

hmi_role_t hmi_security_required_role(hmi_command_t cmd)
{
    if ((int)cmd < 0 || cmd >= HMI_CMD_COUNT) return ROLE_ENGINEER;  /* unknown: most restrictive */
    return s_required_role[cmd];
}

bool hmi_security_authorize(hmi_security_ctx_t *sec, hmi_command_t cmd, uint32_t now_ms)
{
    if (cmd == HMI_CMD_ESTOP) return true;          /* safety function: always allowed */
    if (!sec) return false;
    if (sec->current_role >= hmi_security_required_role(cmd)) return true;
    sec->last_denied_cmd = (uint8_t)cmd;
    sec->last_denied_ms = now_ms;
    sec->denied_count++;
    return false;
}

const char* hmi_security_role_name(hmi_role_t role)
{
    switch (role) {
        case ROLE_VIEWER:      return "Viewer";
        case ROLE_OPERATOR:    return "Operator";
        case ROLE_MAINTENANCE: return "Maintenance";
        case ROLE_ENGINEER:    return "Engineer";
        default:               return "UNKNOWN";
    }
}

const char* hmi_security_command_name(hmi_command_t cmd)
{
    if ((int)cmd < 0 || cmd >= HMI_CMD_COUNT || !s_cmd_names[cmd]) return "COMMAND";
    return s_cmd_names[cmd];
}

bool hmi_security_can_acknowledge_alarm(const hmi_security_ctx_t *sec)
{
    return sec && sec->current_role >= hmi_security_required_role(HMI_CMD_ACK_ALARMS);
}

bool hmi_security_can_operate_equipment(const hmi_security_ctx_t *sec)
{
    return sec && sec->current_role >= hmi_security_required_role(HMI_CMD_PUMP_TOGGLE);
}

bool hmi_security_can_adjust_setpoint(const hmi_security_ctx_t *sec)
{
    return sec && sec->current_role >= ROLE_MAINTENANCE;
}

bool hmi_security_can_configure_system(const hmi_security_ctx_t *sec)
{
    return sec && sec->current_role >= ROLE_ENGINEER;
}

int16_t hmi_security_clamp_setpoint(int16_t requested, int16_t min_val, int16_t max_val)
{
    if (min_val > max_val) { int16_t t = min_val; min_val = max_val; max_val = t; }
    if (requested < min_val) return min_val;
    if (requested > max_val) return max_val;
    return requested;
}

bool hmi_security_validate_packet(const uint8_t *buffer, size_t len, size_t max_allowed)
{
    return buffer != NULL && len != 0 && len <= max_allowed;
}

/* ------------------------------------------------------------------------- */
/* Authentication                                                             */
/* ------------------------------------------------------------------------- */

bool hmi_security_role_needs_pin(hmi_role_t role)
{
    return role > ROLE_VIEWER && role <= ROLE_ENGINEER;
}

static bool pin_valid(const char *pin)
{
    if (!pin) return false;
    size_t n = 0;
    for (; pin[n]; n++) {
        if (pin[n] < '0' || pin[n] > '9' || n >= HMI_PIN_MAX_LEN) return false;
    }
    return n >= HMI_PIN_MIN_LEN;
}

/* Salted, iterated SHA-256: h0 = H(salt || pin), h(i+1) = H(h(i) || salt) */
static void pin_hash(const uint8_t salt[8], const char *pin, uint8_t out[32])
{
    hmi_sha256_t c;
    hmi_sha256_init(&c);
    hmi_sha256_update(&c, salt, 8);
    hmi_sha256_update(&c, pin, strlen(pin));
    hmi_sha256_final(&c, out);
    for (uint32_t i = 1; i < HMI_PIN_HASH_ROUNDS; i++) {
        hmi_sha256_init(&c);
        hmi_sha256_update(&c, out, 32);
        hmi_sha256_update(&c, salt, 8);
        hmi_sha256_final(&c, out);
    }
}

bool hmi_security_set_pin(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, const uint8_t salt[8])
{
    if (!sec || !salt || !hmi_security_role_needs_pin(role) || !pin_valid(pin)) return false;
    memcpy(sec->pins[role].salt, salt, 8);
    pin_hash(salt, pin, sec->pins[role].hash);
    sec->default_pins = false;
    return true;
}

void hmi_security_load_default_pins(hmi_security_ctx_t *sec)
{
    static const char * const pins[HMI_ROLE_COUNT] = { NULL, "1111", "2222", "3333" };
    if (!sec) return;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        uint8_t salt[8] = { 'H', 'M', 'I', 'D', 'E', 'F', 'P', (uint8_t)('0' + r) };
        hmi_security_set_pin(sec, (hmi_role_t)r, pins[r], salt);
    }
    sec->default_pins = true;
}

uint32_t hmi_security_lockout_remaining_ms(hmi_security_ctx_t *sec, uint32_t now_ms)
{
    if (!sec || !sec->locked) return 0;
    uint32_t el = now_ms - sec->lockout_start_ms;
    if (el >= HMI_PIN_LOCKOUT_MS) {
        sec->locked = false;
        sec->failed_attempts = 0;
        return 0;
    }
    return HMI_PIN_LOCKOUT_MS - el;
}

hmi_login_result_t hmi_security_login(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, uint32_t now_ms)
{
    if (!sec || (int)role < (int)ROLE_VIEWER || (int)role > (int)ROLE_ENGINEER) return HMI_LOGIN_INVALID;
    if (role == ROLE_VIEWER) {                      /* logging out never needs a PIN */
        hmi_security_set_role(sec, ROLE_VIEWER, now_ms);
        return HMI_LOGIN_OK;
    }
    if (hmi_security_lockout_remaining_ms(sec, now_ms) > 0) return HMI_LOGIN_LOCKED;
    if (!pin_valid(pin)) return HMI_LOGIN_INVALID;

    uint8_t h[32];
    pin_hash(sec->pins[role].salt, pin, h);
    volatile uint8_t diff = 0;                      /* constant-time compare */
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ sec->pins[role].hash[i]);
    if (diff == 0) {
        sec->failed_attempts = 0;
        hmi_security_set_role(sec, role, now_ms);
        return HMI_LOGIN_OK;
    }
    sec->login_failures_total++;
    if (++sec->failed_attempts >= HMI_PIN_MAX_ATTEMPTS) {
        sec->locked = true;
        sec->lockout_start_ms = now_ms;
        return HMI_LOGIN_LOCKED;
    }
    return HMI_LOGIN_BAD_PIN;
}

/* Vault layout (little-endian):
 *   0  u32 magic "SECV"   4 u8 version (1)   5 u8 record count (3)   6..7 reserved
 *   8  3 x { u8 role, u8 pad[3], u8 salt[8], u8 hash[32] }  (44 bytes each)
 * 140  u32 CRC32-C of bytes 0..139                                         */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t hmi_security_vault_export(const hmi_security_ctx_t *sec, uint8_t *out, size_t cap)
{
    if (!sec || !out || cap < HMI_VAULT_SIZE) return 0;
    memset(out, 0, HMI_VAULT_SIZE);
    put_u32(out, HMI_VAULT_MAGIC);
    out[4] = 1;
    out[5] = 3;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        uint8_t *rec = out + 8 + (r - 1) * 44;
        rec[0] = (uint8_t)r;
        memcpy(rec + 4, sec->pins[r].salt, 8);
        memcpy(rec + 12, sec->pins[r].hash, 32);
    }
    put_u32(out + 140, hmi_crc32c(out, 140));
    return HMI_VAULT_SIZE;
}

bool hmi_security_vault_import(hmi_security_ctx_t *sec, const uint8_t *in, size_t len)
{
    if (!sec || !in || len != HMI_VAULT_SIZE) return false;
    if (get_u32(in) != HMI_VAULT_MAGIC || in[4] != 1 || in[5] != 3) return false;
    if (get_u32(in + 140) != hmi_crc32c(in, 140)) return false;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        if (in[8 + (r - 1) * 44] != (uint8_t)r) return false;
    }
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        const uint8_t *rec = in + 8 + (r - 1) * 44;
        memcpy(sec->pins[r].salt, rec + 4, 8);
        memcpy(sec->pins[r].hash, rec + 12, 32);
    }
    sec->default_pins = false;
    return true;
}
