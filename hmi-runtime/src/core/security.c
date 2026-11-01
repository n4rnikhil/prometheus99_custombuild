/**
 * @file security.c
 * @brief Role-based access control (IEC 62443-4-2 CR 2.1) and input validation
 */

#include "core/security.h"
#include "core/sha256.h"
#include <string.h>

#define DEFAULT_LOGOUT_TIMEOUT_MS  (5u * 60u * 1000u)  /* 5 minutes of inactivity */

static const hmi_role_t s_required_role[HMI_CMD_COUNT] = {
    [HMI_CMD_NAVIGATE]       = ROLE_VIEWER,
    [HMI_CMD_THEME]          = ROLE_VIEWER,
    [HMI_CMD_ROLE_SELECT]    = ROLE_VIEWER,
    [HMI_CMD_ESTOP]          = ROLE_VIEWER,      /* STOP ALL: never gated */
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
    [HMI_CMD_ESTOP]          = "Stop all",
    [HMI_CMD_ACK_ALARMS]     = "Acknowledge",
    [HMI_CMD_SILENCE_HORN]   = "Silence horn",
    [HMI_CMD_PUMP_TOGGLE]    = "Pump start/stop",
    [HMI_CMD_VALVE_TOGGLE]   = "Valve move",
    [HMI_CMD_ESTOP_RESET]    = "Stop-latch reset",
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
    hmi_security_set_device_key(sec, NULL);  /* derived keys from the all-zero key until a real one is set */
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
    if (cmd == HMI_CMD_ESTOP) return true;          /* stop requests are never refused (not a safety function) */
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
    return sec && sec->current_role >= hmi_security_required_role(HMI_CMD_SETPOINT);
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

static bool pin_digits(const char *pin, size_t min_len)
{
    if (!pin) return false;
    size_t n = 0;
    for (; pin[n]; n++) {
        if (pin[n] < '0' || pin[n] > '9' || n >= HMI_PIN_MAX_LEN) return false;
    }
    return n >= min_len;
}

static bool pin_valid(const char *pin) { return pin_digits(pin, HMI_PIN_MIN_LEN); }

/* All one digit (111111) or a straight run up or down (123456, 987654, 890123 wraps too). */
static bool pin_trivial(const char *pin)
{
    size_t n = strlen(pin);
    bool same = true, up = true, down = true;
    for (size_t i = 1; i < n; i++) {
        int a = pin[i - 1] - '0', b = pin[i] - '0';
        if (b != a) same = false;
        if (b != (a + 1) % 10) up = false;
        if (b != (a + 9) % 10) down = false;
    }
    return same || up || down;
}

/* h0 = HMAC(pin_key, salt || pin), h(i+1) = HMAC(pin_key, h(i) || salt). pin_key is derived from
 * the device key, so a copied vault alone cannot be brute-forced. */
static void pin_hash(const uint8_t key[32], const uint8_t salt[8], const char *pin, uint8_t out[32])
{
    hmi_hmac_sha256_t c;
    hmi_hmac_sha256_init(&c, key, 32);
    hmi_hmac_sha256_update(&c, salt, 8);
    hmi_hmac_sha256_update(&c, pin, strlen(pin));
    hmi_hmac_sha256_final(&c, out);
    for (uint32_t i = 1; i < HMI_PIN_HASH_ROUNDS; i++) {
        hmi_hmac_sha256_init(&c, key, 32);
        hmi_hmac_sha256_update(&c, out, 32);
        hmi_hmac_sha256_update(&c, salt, 8);
        hmi_hmac_sha256_final(&c, out);
    }
}

static void update_default_flag(hmi_security_ctx_t *sec)
{
    sec->default_pins = false;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) if (sec->factory[r]) sec->default_pins = true;
}

static void store_pin(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, const uint8_t salt[8], bool factory)
{
    memcpy(sec->pins[role].salt, salt, 8);
    pin_hash(sec->pin_key, salt, pin, sec->pins[role].hash);
    sec->factory[role] = factory;
    update_default_flag(sec);
}

hmi_pin_set_result_t hmi_security_set_pin(hmi_security_ctx_t *sec, hmi_role_t role, const char *pin, const uint8_t salt[8])
{
    if (!sec || !salt || !hmi_security_role_needs_pin(role)) return HMI_PIN_SET_BAD_ROLE;
    if (!pin_digits(pin, HMI_PIN_SITE_MIN_LEN)) return HMI_PIN_SET_FORMAT;
    if (pin_trivial(pin)) return HMI_PIN_SET_TRIVIAL;
    store_pin(sec, role, pin, salt, false);
    return HMI_PIN_SET_OK;
}

const char *hmi_security_pin_set_message(hmi_pin_set_result_t r)
{
    switch (r) {
        case HMI_PIN_SET_OK:       return "PIN set";
        case HMI_PIN_SET_BAD_ROLE: return "unknown role (operator, maintenance or engineer)";
        case HMI_PIN_SET_FORMAT:   return "a site PIN has 6 to 8 digits";
        case HMI_PIN_SET_TRIVIAL:  return "PIN too easy to guess (one repeated digit or a straight run)";
        default:                   return "PIN not accepted";
    }
}

static const char * const s_factory_pins[HMI_ROLE_COUNT] = { NULL, "1111", "2222", "3333" };

static void factory_salt(int r, uint8_t salt[8])
{
    const uint8_t s[8] = { 'H', 'M', 'I', 'D', 'E', 'F', 'P', (uint8_t)('0' + r) };
    memcpy(salt, s, 8);
}

void hmi_security_load_default_pins(hmi_security_ctx_t *sec)
{
    if (!sec) return;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        uint8_t salt[8];
        factory_salt(r, salt);
        store_pin(sec, (hmi_role_t)r, s_factory_pins[r], salt, true);
    }
}

void hmi_security_set_device_key(hmi_security_ctx_t *sec, const uint8_t *key32)
{
    if (!sec) return;
    static const uint8_t zero[32];
    const uint8_t *k = key32 ? key32 : zero;
    hmi_hkdf_sha256(NULL, 0, k, 32, "pin-hash", 8, sec->pin_key, sizeof(sec->pin_key));
    hmi_hkdf_sha256(NULL, 0, k, 32, "vault-mac", 9, sec->vault_key, sizeof(sec->vault_key));
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {     /* factory PINs follow the new key */
        if (sec->factory[r]) {
            uint8_t salt[8];
            factory_salt(r, salt);
            store_pin(sec, (hmi_role_t)r, s_factory_pins[r], salt, true);
        }
    }
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
    /* Commissioning gate: no control with a published factory PIN (IEC 62443-4-2 CR 1.5) */
    if (sec->factory[role] && !sec->allow_factory_pins) return HMI_LOGIN_FACTORY_PIN;

    uint8_t h[32];
    pin_hash(sec->pin_key, sec->pins[role].salt, pin, h);
    if (hmi_ct_equal(h, sec->pins[role].hash, 32)) {
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

/* Vault layout v2 (little-endian):
 *   0  u32 magic "SECV"   4 u8 version (2)   5 u8 record count (3)   6..7 reserved
 *   8  3 x { u8 role, u8 factory, u8 pad[2], u8 salt[8], u8 hash[32] }  (44 bytes each)
 * 140  HMAC-SHA256(device key, bytes 0..139)                                (32 bytes)
 * v1 (144 bytes, CRC32-C instead of a signature) is refused: anyone could write one.   */
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define VAULT_BODY 140u

size_t hmi_security_vault_export(const hmi_security_ctx_t *sec, uint8_t *out, size_t cap)
{
    if (!sec || !out || cap < HMI_VAULT_SIZE) return 0;
    memset(out, 0, HMI_VAULT_SIZE);
    put_u32(out, HMI_VAULT_MAGIC);
    out[4] = HMI_VAULT_VERSION;
    out[5] = 3;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        uint8_t *rec = out + 8 + (r - 1) * 44;
        rec[0] = (uint8_t)r;
        rec[1] = sec->factory[r] ? 1u : 0u;
        memcpy(rec + 4, sec->pins[r].salt, 8);
        memcpy(rec + 12, sec->pins[r].hash, 32);
    }
    hmi_hmac_sha256(sec->vault_key, sizeof(sec->vault_key), out, VAULT_BODY, out + VAULT_BODY);
    return HMI_VAULT_SIZE;
}

hmi_vault_result_t hmi_security_vault_import(hmi_security_ctx_t *sec, const uint8_t *in, size_t len)
{
    if (!sec || !in) return HMI_VAULT_DAMAGED;
    if (len == HMI_VAULT_V1_SIZE && get_u32(in) == HMI_VAULT_MAGIC && in[4] == 1) return HMI_VAULT_LEGACY;
    if (len != HMI_VAULT_SIZE || get_u32(in) != HMI_VAULT_MAGIC || in[4] != HMI_VAULT_VERSION || in[5] != 3)
        return HMI_VAULT_DAMAGED;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        if (in[8 + (r - 1) * 44] != (uint8_t)r || in[9 + (r - 1) * 44] > 1u) return HMI_VAULT_DAMAGED;
    }
    uint8_t mac[32];
    hmi_hmac_sha256(sec->vault_key, sizeof(sec->vault_key), in, VAULT_BODY, mac);
    if (!hmi_ct_equal(mac, in + VAULT_BODY, 32)) return HMI_VAULT_AUTH_FAILED;
    for (int r = ROLE_OPERATOR; r <= ROLE_ENGINEER; r++) {
        const uint8_t *rec = in + 8 + (r - 1) * 44;
        sec->factory[r] = rec[1] != 0;
        memcpy(sec->pins[r].salt, rec + 4, 8);
        memcpy(sec->pins[r].hash, rec + 12, 32);
    }
    update_default_flag(sec);
    return HMI_VAULT_OK;
}
