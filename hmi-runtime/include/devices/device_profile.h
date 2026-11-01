/**
 * @file device_profile.h
 * @brief Plug-and-play driver modules: everything the HMI needs to know about a PLC type
 *
 * A driver module is pure const data (portable C99, lives in flash on an MCU):
 *   - how to recognise the device (Modbus FC43 vendor / product name)
 *   - its tags (metadata, relative ids 0 .. HMI_DEVICE_TAG_SPAN-1)
 *   - which Modbus registers / coils feed which tags
 *   - the operator controls: start / stop, setpoints, mode selectors, resets
 *   - two dashboard layouts: a full one and a compact one for small panels
 *
 * When the discovery thread finds a device, the device manager picks the first module
 * that matches, gives it a free slot (tag block HMI_DEV_TAG(slot, 0..23)) and the
 * dashboard draws its layout. Its controls are on the dashboard (quick buttons) and in
 * the device faceplate. Adding a new PLC type = adding one module.
 */

#ifndef INDUSTRIAL_HMI_DEVICE_PROFILE_H
#define INDUSTRIAL_HMI_DEVICE_PROFILE_H

#include "config.h"
#include "core/tags.h"

#define HMI_REL_NONE    0xFFu
#define HMI_REL_DEVICE  (HMI_DEVICE_TAG_SPAN - 1)   /* pseudo tag in manifests: "this device" (card title = device name) */
#define HMI_MAX_ACTIONS 16
#define HMI_MAX_OPTIONS 4                           /* choices of a mode selector */

typedef enum {
    HMI_ACT_NONE = 0,
    HMI_ACT_TOGGLE_COIL,    /* write !current to a coil */
    HMI_ACT_TOGGLE_REG,     /* current > threshold ? value_low : value_high, written to a holding register */
    HMI_ACT_COIL_ON,        /* write 1 to a coil ("Start", "Open") - refused while block_tag is set */
    HMI_ACT_COIL_OFF,       /* write 0 to a coil ("Stop", "Close") - never blocked */
    HMI_ACT_PULSE_COIL,     /* write 1, then 0 after HMI_PULSE_MS ("Reset fault", "Reset counter") */
    HMI_ACT_SET_REG,        /* operator enters a value (numeric keypad) within [min, max], written to a register */
    HMI_ACT_SET_ENUM,       /* operator picks one of `options` ("Manual|Auto"); the option index is written */
    /* Command-word devices (drives): fixed values written to one register, e.g. Altivar CMD 8501 */
    HMI_ACT_REG_ON,         /* "Run": writes seq[0..seq_len) in order - refused while block_tag is set */
    HMI_ACT_REG_OFF,        /* "Stop": writes seq[0..seq_len) in order - never blocked */
    HMI_ACT_PULSE_REG       /* "Reset fault": writes seq[0], then seq[1] after HMI_PULSE_MS */
} hmi_action_kind_t;

#define HMI_PULSE_MS 500u
#define HMI_MAX_SEQ  4      /* values in a command-word sequence */

/* Start-like, stop-like and pulse-like controls behave the same whether they write a coil
 * or a command word: the UI and the quick buttons only care about this. */
static inline bool hmi_act_is_start(uint8_t k) { return k == HMI_ACT_COIL_ON || k == HMI_ACT_REG_ON; }
static inline bool hmi_act_is_stop(uint8_t k)  { return k == HMI_ACT_COIL_OFF || k == HMI_ACT_REG_OFF; }
static inline bool hmi_act_is_pulse(uint8_t k) { return k == HMI_ACT_PULSE_COIL || k == HMI_ACT_PULSE_REG; }
static inline bool hmi_act_writes_coil(uint8_t k)
{
    return k == HMI_ACT_TOGGLE_COIL || k == HMI_ACT_COIL_ON || k == HMI_ACT_COIL_OFF || k == HMI_ACT_PULSE_COIL;
}

typedef struct {
    uint8_t     rel_tag;      /* the tag this control acts on (its value shows the current state) */
    uint8_t     kind;         /* hmi_action_kind_t */
    uint8_t     command;      /* hmi_command_t checked by RBAC */
    uint8_t     block_tag;    /* turning ON is refused while this rel tag is non-zero (e.g. E-stop); HMI_REL_NONE = none */
    uint16_t    address;      /* 0-based coil or holding register */
    int16_t     threshold;    /* TOGGLE_REG only */
    int16_t     value_low;    /* TOGGLE_REG only */
    int16_t     value_high;   /* TOGGLE_REG only */
    const char *label_on;     /* TOGGLE kinds: label while on / above threshold */
    const char *label;        /* button label: "Start", "Stop", "Set", "Reset"; TOGGLE kinds: label while off */
    const char *log_name;     /* "Pump A" -> journal "Pump A start commanded" */
    int16_t     min, max;     /* SET_REG: accepted raw range (the PLC must clamp as well) */
    const char *options;      /* SET_ENUM: "Off|Hand|Auto" (index 0, 1, 2 is written) */
    bool        confirm;      /* ask "Are you sure?" before sending */
    const int16_t *seq;       /* REG_ON / REG_OFF / PULSE_REG: values written to `address` */
    uint8_t     seq_len;      /* 1..HMI_MAX_SEQ */
} hmi_device_action_t;

/* Constructors for driver modules (designated initialisers; need core/security.h for HMI_CMD_*) */
#define HMI_A_START(rel, coil, blk, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_COIL_ON, .command = HMI_CMD_OPERATE, .block_tag = (blk), \
      .address = (coil), .label = (lbl), .log_name = (log), .confirm = (conf) }
#define HMI_A_STOP(rel, coil, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_COIL_OFF, .command = HMI_CMD_OPERATE, .block_tag = HMI_REL_NONE, \
      .address = (coil), .label = (lbl), .log_name = (log), .confirm = (conf) }
#define HMI_A_PULSE(rel, coil, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_PULSE_COIL, .command = HMI_CMD_OPERATE, .block_tag = HMI_REL_NONE, \
      .address = (coil), .label = (lbl), .log_name = (log), .confirm = (conf) }
#define HMI_A_SET(rel, reg, cmd, lbl, log, mn, mx) \
    { .rel_tag = (rel), .kind = HMI_ACT_SET_REG, .command = (cmd), .block_tag = HMI_REL_NONE, \
      .address = (reg), .label = (lbl), .log_name = (log), .min = (mn), .max = (mx) }
#define HMI_A_MODE(rel, reg, opts, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_SET_ENUM, .command = HMI_CMD_OPERATE, .block_tag = HMI_REL_NONE, \
      .address = (reg), .label = "Mode", .log_name = (log), .options = (opts), .confirm = (conf) }
/* Command-word controls; `seq` is a static const int16_t array */
#define HMI_SEQ_LEN(s) ((uint8_t)(sizeof(s) / sizeof((s)[0])))
#define HMI_A_RUN_WORD(rel, reg, blk, s, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_REG_ON, .command = HMI_CMD_OPERATE, .block_tag = (blk), \
      .address = (reg), .label = (lbl), .log_name = (log), .confirm = (conf), .seq = (s), .seq_len = HMI_SEQ_LEN(s) }
#define HMI_A_STOP_WORD(rel, reg, s, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_REG_OFF, .command = HMI_CMD_OPERATE, .block_tag = HMI_REL_NONE, \
      .address = (reg), .label = (lbl), .log_name = (log), .confirm = (conf), .seq = (s), .seq_len = HMI_SEQ_LEN(s) }
#define HMI_A_PULSE_WORD(rel, reg, s, lbl, log, conf) \
    { .rel_tag = (rel), .kind = HMI_ACT_PULSE_REG, .command = HMI_CMD_OPERATE, .block_tag = HMI_REL_NONE, \
      .address = (reg), .label = (lbl), .log_name = (log), .confirm = (conf), .seq = (s), .seq_len = HMI_SEQ_LEN(s) }

/* How the registers of a block become tag values */
typedef enum {
    HMI_FMT_INT16 = 0,      /* one register = one signed 16-bit value (default) */
    HMI_FMT_FLOAT32         /* two registers = one IEEE-754 float, high word first (PowerLogic meters);
                               the tag value is float x 10^decimals, rounded and clamped to int16 */
} hmi_reg_format_t;

typedef struct {
    uint16_t       start;     /* first 0-based address */
    uint8_t        count;     /* registers / coils read in one request (0 = none) */
    const uint8_t *rel_tags;  /* rel tag for each item (FLOAT32: per register pair), HMI_REL_NONE to skip */
    uint8_t        format;    /* hmi_reg_format_t, registers only */
} hmi_block_map_t;

/* A digital tag derived from bits of a register tag: dst = ((src & mask) == match).
 * E.g. Altivar status word ETA: running = (ETA & 0x006F) == 0x0027, fault = (ETA & 0x0008) == 0x0008 */
typedef struct {
    uint8_t  src_rel;
    uint8_t  dst_rel;
    uint16_t mask;
    uint16_t match;
} hmi_bit_map_t;

typedef struct {
    const char *key;              /* "tank_skid" */
    const char *name;             /* "Tank skid"  (device label = name + slot number) */
    const char *match_vendor;     /* case-insensitive substring of the FC43 vendor name; NULL = any */
    const char *match_product;    /* case-insensitive substring(s) of the FC43 product code, "a|b" = either; NULL = any */
    bool        needs_identity;   /* true = only match devices that answered FC43 */
    bool        auto_attach;      /* true = discovery may attach it on its own (a real PLC / drive driver);
                                     false = only for addresses the user configured with --plc */

    const hmi_tag_meta_t *meta;   /* tag_id fields are RELATIVE ids */
    uint8_t               meta_count;

    hmi_block_map_t regs;         /* FC03 read */
    hmi_block_map_t coils;        /* FC01 read */

    const hmi_device_action_t *actions;
    uint8_t                    n_actions;

    uint8_t  comm_tag;            /* rel digital tag the manager sets while the link is down (CRITICAL alarm) */
    uint8_t  estop_tag;           /* rel tag showing the E-stop state, HMI_REL_NONE = no E-stop */
    uint16_t estop_coil;          /* the program's stop latch: STOP ALL writes 1, Reset stop writes 0; 0xFFFF = none.
                                   * The hardwired E-stop chain also sets it inside the PLC; the HMI never
                                   * replaces that chain. */
    uint8_t  trend_tag;           /* rel tag sampled into this slot's 60 s trend; HMI_REL_NONE = none */

    const uint32_t *ui_full;      /* manifest for a large panel */
    uint8_t         n_full;
    const uint32_t *ui_compact;   /* manifest for a small panel (dashboard with several devices) */
    uint8_t         n_compact;

    /* --- optional (zero = unused) --- */
    const hmi_block_map_t *more_regs;   /* further FC03 blocks; an exception on one of them only marks its
                                           own tags bad (the device stays online) */
    uint8_t                n_more_regs;
    const hmi_bit_map_t   *bits;        /* digital tags derived from register bits (status words) */
    uint8_t                n_bits;
    /* Application modules (a PLC program, not a fixed product): the device must also report this
     * text in its FC43 UserApplicationName (object 0x06) before discovery attaches it on its own.
     * A real PLC running some other program is then never driven with a guessed register map;
     * attach it deliberately with --plc <ip>@<module key>. NULL = fixed product map (drives, meters). */
    const char            *match_app;
} hmi_device_profile_t;

/* Built-in driver modules (src/devices/), most specific first in the registry */
extern const hmi_device_profile_t g_profile_em6400;
extern const hmi_device_profile_t g_profile_tank_skid;
extern const hmi_device_profile_t g_profile_altivar;
extern const hmi_device_profile_t g_profile_conveyor;
extern const hmi_device_profile_t g_profile_boiler;
extern const hmi_device_profile_t g_profile_pump_station;
extern const hmi_device_profile_t g_profile_compressor;
extern const hmi_device_profile_t g_profile_ahu;
extern const hmi_device_profile_t g_profile_abb_drive;
extern const hmi_device_profile_t g_profile_generic;

uint8_t hmi_profile_count(void);
const hmi_device_profile_t* hmi_profile_at(uint8_t idx);

/* First module matching an identity. vendor/product/app may be NULL or "" when the
 * device did not answer FC43 (app = UserApplicationName, object 0x06). Never returns NULL
 * (the generic module matches anything). */
const hmi_device_profile_t* hmi_profile_match(const char *vendor, const char *product, const char *app);
/* Module by key ("altivar", "em6400", "generic", ...), NULL if not compiled in */
const hmi_device_profile_t* hmi_profile_by_key(const char *key);

/* First action for a relative tag, or NULL / -1 */
const hmi_device_action_t* hmi_profile_action(const hmi_device_profile_t *p, uint8_t rel_tag);
int hmi_profile_action_index(const hmi_device_profile_t *p, uint8_t rel_tag);
/* The action a one-tap button on that tag performs in the current state: Stop while
 * running, Start while stopped, otherwise the first action of the tag. -1 = none. */
int hmi_profile_quick_action(const hmi_device_profile_t *p, uint8_t rel_tag, int16_t current);
/* Mode selector of a tag (SET_ENUM), used to show its value as text; NULL if none */
const hmi_device_action_t* hmi_profile_enum_action(const hmi_device_profile_t *p, uint8_t rel_tag);
/* Target value a TOGGLE_REG action would write for the current value */
int16_t hmi_action_reg_target(const hmi_device_action_t *a, int16_t current);
/* SET_ENUM options: count (0 for other kinds) and the label of option idx ("" if none) */
uint8_t hmi_action_option_count(const hmi_device_action_t *a);
void    hmi_action_option_label(const hmi_device_action_t *a, uint8_t idx, char *out, size_t cap);

/* --- Faceplate: a device's controls grouped by the tag they act on --- */
#define HMI_FP_MAX_ROWS  7
#define HMI_FP_MAX_CTRLS 20
#define HMI_FP_NO_OPTION 0xFFu
typedef struct {
    uint8_t row;       /* faceplate row */
    uint8_t action;    /* index into profile->actions */
    uint8_t option;    /* SET_ENUM option index, else HMI_FP_NO_OPTION */
} hmi_fp_ctrl_t;

/* Rows = distinct action tags in action order (at most HMI_FP_MAX_ROWS). */
uint8_t hmi_profile_fp_rows(const hmi_device_profile_t *p, uint8_t rel_out[HMI_FP_MAX_ROWS]);
/* Buttons in row order: one per action, one per option of a mode selector. */
uint8_t hmi_profile_fp_controls(const hmi_device_profile_t *p, hmi_fp_ctrl_t *out, uint8_t cap);

#endif /* INDUSTRIAL_HMI_DEVICE_PROFILE_H */
