/**
 * @file profile_generic.c
 * @brief Driver module: generic Modbus device (only for explicitly configured --plc targets)
 *
 * A driver module is pure const data (portable C99): identification, tags,
 * Modbus map, operator actions, alarms and its own screen layouts. The core HMI
 * knows nothing about the device; the module is only used while such a device
 * is connected. Register it in device_profiles.c.
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

/* ========================================================================= */
/* Generic Modbus device (anything that answers FC03 but is not recognised)   */
/* ========================================================================= */
enum { GN_HR0 = 0, GN_COMM = 8 };

static const hmi_tag_meta_t s_generic_meta[] = {
    { 0, TAG_TYPE_ANALOG, "HR 0", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 1, TAG_TYPE_ANALOG, "HR 1", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 2, TAG_TYPE_ANALOG, "HR 2", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 3, TAG_TYPE_ANALOG, "HR 3", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 4, TAG_TYPE_ANALOG, "HR 4", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 5, TAG_TYPE_ANALOG, "HR 5", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 6, TAG_TYPE_ANALOG, "HR 6", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { 7, TAG_TYPE_ANALOG, "HR 7", "", 0, 1000, 0, 0, 0, 0, NULL, NULL, NULL },
    { GN_COMM, TAG_TYPE_DIGITAL, "Device comm loss", "", 0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "Check the Ethernet cable and device power", "Verify the device address and port", NULL },
};

static const uint8_t s_generic_regs[] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static const uint32_t s_generic_full[] = {
    HMI_TOKEN_PACK(HMI_REL_DEVICE, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(0, WIDGET_ANALOG_BAR, 0, 2, 8, 2),
    HMI_TOKEN_PACK(1, WIDGET_ANALOG_BAR, 8, 2, 8, 2),
    HMI_TOKEN_PACK(2, WIDGET_ANALOG_BAR, 0, 5, 8, 2),
    HMI_TOKEN_PACK(3, WIDGET_ANALOG_BAR, 8, 5, 8, 2),
    HMI_TOKEN_PACK(4, WIDGET_ANALOG_BAR, 0, 8, 8, 2),
    HMI_TOKEN_PACK(5, WIDGET_ANALOG_BAR, 8, 8, 8, 2),
    HMI_TOKEN_PACK(6, WIDGET_ANALOG_BAR, 0, 11, 8, 2),
    HMI_TOKEN_PACK(7, WIDGET_ANALOG_BAR, 8, 11, 8, 2),
};

static const uint32_t s_generic_compact[] = {
    HMI_TOKEN_PACK(HMI_REL_DEVICE, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(0, WIDGET_ANALOG_BAR, 0, 2, 0, 2),
    HMI_TOKEN_PACK(1, WIDGET_ANALOG_BAR, 0, 5, 0, 2),
    HMI_TOKEN_PACK(2, WIDGET_ANALOG_BAR, 0, 8, 0, 2),
    HMI_TOKEN_PACK(3, WIDGET_ANALOG_BAR, 0, 11, 0, 2),
};

const hmi_device_profile_t g_profile_generic = {
    "generic", "Modbus device", NULL, NULL, false, false,
    s_generic_meta, (uint8_t)(sizeof(s_generic_meta) / sizeof(s_generic_meta[0])),
    { 0, (uint8_t)sizeof(s_generic_regs), s_generic_regs },
    { 0, 0, NULL },
    NULL, 0,
    GN_COMM, HMI_REL_NONE, 0xFFFFu, GN_HR0,
    s_generic_full, (uint8_t)(sizeof(s_generic_full) / sizeof(s_generic_full[0])),
    s_generic_compact, (uint8_t)(sizeof(s_generic_compact) / sizeof(s_generic_compact[0])),
};
