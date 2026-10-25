/**
 * @file device_profile.h
 * @brief Plug-and-play device profiles: everything the HMI needs to know about a PLC type
 *
 * A profile is pure const data (portable C99, lives in flash on an MCU):
 *   - how to recognise the device (Modbus FC43 vendor / product name)
 *   - its tags (metadata, relative ids 0 .. HMI_DEVICE_TAG_SPAN-1)
 *   - which Modbus registers / coils feed which tags
 *   - the operator actions its buttons perform
 *   - two UI manifests: a full layout and a compact one for small panels
 *
 * When the discovery thread finds a device, the device manager picks the first
 * profile that matches, gives it a free slot (tag block HMI_DEV_TAG(slot, 0..23))
 * and the dashboard draws its manifest. Adding a new PLC type = adding one profile.
 */

#ifndef INDUSTRIAL_HMI_DEVICE_PROFILE_H
#define INDUSTRIAL_HMI_DEVICE_PROFILE_H

#include "../config.h"
#include "../core/tags.h"

#define HMI_REL_NONE    0xFFu
#define HMI_REL_DEVICE  (HMI_DEVICE_TAG_SPAN - 1)   /* pseudo tag in manifests: "this device" (card title = device name) */
#define HMI_MAX_ACTIONS 4

typedef enum {
    HMI_ACT_NONE = 0,
    HMI_ACT_TOGGLE_COIL,    /* write !current to a coil (start / stop) */
    HMI_ACT_TOGGLE_REG      /* current > threshold ? value_low : value_high, written to a holding register */
} hmi_action_kind_t;

typedef struct {
    uint8_t     rel_tag;      /* the button's tag; its value decides the toggle direction */
    uint8_t     kind;         /* hmi_action_kind_t */
    uint8_t     command;      /* hmi_command_t checked by RBAC */
    uint8_t     block_tag;    /* turning ON is refused while this rel tag is non-zero (e.g. E-stop); HMI_REL_NONE = none */
    uint16_t    address;      /* 0-based coil or holding register */
    int16_t     threshold;
    int16_t     value_low;
    int16_t     value_high;
    const char *label_on;     /* button label when the tag is on / above threshold, e.g. "Stop pump" */
    const char *label_off;    /* ... otherwise, e.g. "Start pump". TOGGLE_REG labels may contain %s = target value */
    const char *log_name;     /* "Pump A" -> journal "Pump A start commanded" */
} hmi_device_action_t;

typedef struct {
    uint16_t       start;     /* first 0-based address */
    uint8_t        count;     /* registers / coils read in one request (0 = none) */
    const uint8_t *rel_tags;  /* rel tag for each item, HMI_REL_NONE to skip */
} hmi_block_map_t;

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
    uint16_t estop_coil;          /* coil written by the HMI E-STOP (1 = trip, 0 = reset); 0xFFFF = none */
    uint8_t  trend_tag;           /* rel tag sampled into this slot's 60 s trend; HMI_REL_NONE = none */

    const uint32_t *ui_full;      /* manifest for a large panel */
    uint8_t         n_full;
    const uint32_t *ui_compact;   /* manifest for a small panel (dashboard with several devices) */
    uint8_t         n_compact;
} hmi_device_profile_t;

/* Built-in profiles (device_profiles.c), most specific first */
extern const hmi_device_profile_t g_profile_drive;
extern const hmi_device_profile_t g_profile_tank_skid;
extern const hmi_device_profile_t g_profile_generic;

uint8_t hmi_profile_count(void);
const hmi_device_profile_t* hmi_profile_at(uint8_t idx);

/* First profile matching an identity. vendor/product may be NULL or "" when the
 * device did not answer FC43. Never returns NULL (the generic profile matches anything). */
const hmi_device_profile_t* hmi_profile_match(const char *vendor, const char *product);

/* Action for a relative tag, or NULL */
const hmi_device_action_t* hmi_profile_action(const hmi_device_profile_t *p, uint8_t rel_tag);
/* Index of that action (0..n_actions-1) or -1 */
int hmi_profile_action_index(const hmi_device_profile_t *p, uint8_t rel_tag);
/* Target value a TOGGLE_REG action would write for the current value */
int16_t hmi_action_reg_target(const hmi_device_action_t *a, int16_t current);

#endif /* INDUSTRIAL_HMI_DEVICE_PROFILE_H */
