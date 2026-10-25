/**
 * @file device_profiles.c
 * @brief Driver registry and identity matching (the driver modules live in profiles/)
 *
 * Register maps are 0-based Modbus addresses. On a Modicon M340/M580, holding
 * register n is %MWn and coil n is %Mn; the PLC program decides what lives there,
 * so a profile describes an APPLICATION (tank skid, drive, ...) rather than a CPU.
 * The drive profile uses the register layout of tests/modbus_plc_sim.py --profile drive;
 * a real Altivar needs its own map (e.g. RFR 3202, LCR 3204, CMD 8501) in a new profile.
 */

#include "../../include/drivers/device_profile.h"
#include "../../include/core/security.h"
#include "../../include/renderer/bytecode.h"
#include <string.h>

/* ========================================================================= */
/* Matching                                                                   */
/* ========================================================================= */
static const hmi_device_profile_t * const s_profiles[] = {
    &g_profile_drive, &g_profile_tank_skid, &g_profile_generic
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

int16_t hmi_action_reg_target(const hmi_device_action_t *a, int16_t current)
{
    if (!a) return 0;
    return (current > a->threshold) ? a->value_low : a->value_high;
}
