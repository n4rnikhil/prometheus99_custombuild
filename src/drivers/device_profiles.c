/**
 * @file device_profiles.c
 * @brief Driver-module registry, identity matching and control grouping (modules live in profiles/)
 *
 * Register maps are 0-based Modbus addresses. On a Modicon PLC, holding register n is
 * %MWn and coil n is %Mn; the PLC program decides what lives there, so a module
 * describes an APPLICATION (tank skid, conveyor, boiler, ...) on a given controller.
 * The maps match the devices of PLC Lab (lab/), which simulate those applications.
 */

#include "../../include/drivers/device_profile.h"
#include "../../include/core/security.h"
#include "../../include/renderer/bytecode.h"
#include <stdio.h>
#include <string.h>

/* ========================================================================= */
/* Registry: most specific identity first; the generic module always last     */
/* ========================================================================= */
static const hmi_device_profile_t * const s_profiles[] = {
#if HMI_DRIVER_DRIVE
    &g_profile_drive,
#endif
#if HMI_DRIVER_TANK_SKID
    &g_profile_tank_skid,
#endif
#if HMI_DRIVER_CONVEYOR
    &g_profile_conveyor,
#endif
#if HMI_DRIVER_BOILER
    &g_profile_boiler,
#endif
#if HMI_DRIVER_PUMP_STATION
    &g_profile_pump_station,
#endif
#if HMI_DRIVER_COMPRESSOR
    &g_profile_compressor,
#endif
#if HMI_DRIVER_AHU
    &g_profile_ahu,
#endif
#if HMI_DRIVER_ABB_DRIVE
    &g_profile_abb_drive,
#endif
    &g_profile_generic
};

uint8_t hmi_profile_count(void)
{
    return (uint8_t)(sizeof(s_profiles) / sizeof(s_profiles[0]));
}

const hmi_device_profile_t* hmi_profile_at(uint8_t idx)
{
    return idx < hmi_profile_count() ? s_profiles[idx] : NULL;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

/* case-insensitive "needle in haystack"; NULL needle matches anything */
static bool contains_one(const char *hay, const char *needle, size_t nlen)
{
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nlen && hay[i] && lower(hay[i]) == lower(needle[i])) i++;
        if (i == nlen) return true;
    }
    return false;
}

/* case-insensitive substring match; "a|b|c" matches any alternative; NULL/"" matches anything */
static bool contains(const char *hay, const char *needles)
{
    if (!needles || !needles[0]) return true;
    if (!hay || !hay[0]) return false;
    const char *p = needles;
    while (*p) {
        size_t n = strcspn(p, "|");
        if (n && contains_one(hay, p, n)) return true;
        p += n;
        if (*p == '|') p++;
    }
    return false;
}

const hmi_device_profile_t* hmi_profile_match(const char *vendor, const char *product)
{
    bool identified = (vendor && vendor[0]) || (product && product[0]);
    for (uint8_t i = 0; i < hmi_profile_count(); i++) {
        const hmi_device_profile_t *p = s_profiles[i];
        if (p->needs_identity && !identified) continue;
        if (contains(vendor, p->match_vendor) && contains(product, p->match_product)) return p;
    }
    return &g_profile_generic;
}

/* ========================================================================= */
/* Actions                                                                    */
/* ========================================================================= */
const hmi_device_action_t* hmi_profile_action(const hmi_device_profile_t *p, uint8_t rel_tag)
{
    int i = hmi_profile_action_index(p, rel_tag);
    return i < 0 ? NULL : &p->actions[i];
}

int hmi_profile_action_index(const hmi_device_profile_t *p, uint8_t rel_tag)
{
    if (!p) return -1;
    for (uint8_t i = 0; i < p->n_actions; i++) {
        if (p->actions[i].rel_tag == rel_tag) return i;
    }
    return -1;
}

int hmi_profile_quick_action(const hmi_device_profile_t *p, uint8_t rel_tag, int16_t current)
{
    if (!p) return -1;
    int first = -1;
    for (uint8_t i = 0; i < p->n_actions; i++) {
        const hmi_device_action_t *a = &p->actions[i];
        if (a->rel_tag != rel_tag) continue;
        if (first < 0) first = i;
        if (a->kind == HMI_ACT_COIL_OFF && current != 0) return i;   /* running: the button stops it */
        if (a->kind == HMI_ACT_COIL_ON && current == 0) return i;    /* stopped: the button starts it */
    }
    return first;
}

const hmi_device_action_t* hmi_profile_enum_action(const hmi_device_profile_t *p, uint8_t rel_tag)
{
    if (!p) return NULL;
    for (uint8_t i = 0; i < p->n_actions; i++)
        if (p->actions[i].rel_tag == rel_tag && p->actions[i].kind == HMI_ACT_SET_ENUM) return &p->actions[i];
    return NULL;
}

int16_t hmi_action_reg_target(const hmi_device_action_t *a, int16_t current)
{
    if (!a) return 0;
    return (current > a->threshold) ? a->value_low : a->value_high;
}

uint8_t hmi_action_option_count(const hmi_device_action_t *a)
{
    if (!a || a->kind != HMI_ACT_SET_ENUM || !a->options || !a->options[0]) return 0;
    uint8_t n = 1;
    for (const char *c = a->options; *c; c++) if (*c == '|') n++;
    return n > HMI_MAX_OPTIONS ? HMI_MAX_OPTIONS : n;
}

void hmi_action_option_label(const hmi_device_action_t *a, uint8_t idx, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (idx >= hmi_action_option_count(a)) return;
    const char *p = a->options;
    for (uint8_t i = 0; i < idx; i++) p = strchr(p, '|') + 1;
    size_t n = strcspn(p, "|");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* ========================================================================= */
/* Faceplate grouping                                                         */
/* ========================================================================= */
uint8_t hmi_profile_fp_rows(const hmi_device_profile_t *p, uint8_t rel_out[HMI_FP_MAX_ROWS])
{
    uint8_t n = 0;
    if (!p) return 0;
    for (uint8_t i = 0; i < p->n_actions && n < HMI_FP_MAX_ROWS; i++) {
        uint8_t rel = p->actions[i].rel_tag;
        bool seen = false;
        for (uint8_t k = 0; k < n; k++) if (rel_out[k] == rel) seen = true;
        if (!seen) rel_out[n++] = rel;
    }
    return n;
}

uint8_t hmi_profile_fp_controls(const hmi_device_profile_t *p, hmi_fp_ctrl_t *out, uint8_t cap)
{
    uint8_t rows[HMI_FP_MAX_ROWS];
    uint8_t nrows = hmi_profile_fp_rows(p, rows);
    uint8_t n = 0;
    for (uint8_t r = 0; r < nrows; r++) {
        for (uint8_t i = 0; i < p->n_actions; i++) {
            const hmi_device_action_t *a = &p->actions[i];
            if (a->rel_tag != rows[r]) continue;
            uint8_t opts = hmi_action_option_count(a);
            if (opts == 0) {
                if (n < cap) { out[n].row = r; out[n].action = i; out[n].option = HMI_FP_NO_OPTION; n++; }
            } else {
                for (uint8_t o = 0; o < opts && n < cap; o++) { out[n].row = r; out[n].action = i; out[n].option = o; n++; }
            }
        }
    }
    return n;
}
