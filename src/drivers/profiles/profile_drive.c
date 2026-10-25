/**
 * @file profile_drive.c
 * @brief Driver module: Schneider Altivar variable-speed drive (ATV)
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
/* Variable-speed drive                                                       */
/* ========================================================================= */
enum { DR_FREQ = 0, DR_CURRENT, DR_SETPOINT, DR_TEMP, DR_RUN, DR_FAULT, DR_COMM = 8 };

static const hmi_tag_meta_t s_drive_meta[] = {
    { DR_FREQ,     TAG_TYPE_ANALOG,  "Output frequency", "Hz", 0, 600,   0,   0, 1, 0,                  NULL, NULL, NULL },
    { DR_CURRENT,  TAG_TYPE_ANALOG,  "Motor current",    "A",  0, 300, 220, 260, 1, 0,                  "Check the load for jamming",            "Reduce the speed setpoint", NULL },
    { DR_SETPOINT, TAG_TYPE_ANALOG,  "Speed setpoint",   "Hz", 0, 600,   0,   0, 1, 0,                  NULL, NULL, NULL },
    { DR_TEMP,     TAG_TYPE_ANALOG,  "Drive temp",       "C",  0, 120,  70,  85, 0, 0,                  "Check the cabinet fan and filters",     "Reduce the motor load", NULL },
    { DR_RUN,      TAG_TYPE_DIGITAL, "Drive running",    "",   0, 1,     0,   0, 0, 0,                  NULL, NULL, NULL },
    { DR_FAULT,    TAG_TYPE_DIGITAL, "Drive fault",      "",   0, 1,     0,   0, 0, ALARM_SEV_CRITICAL, "Read the fault code on the drive panel", "Reset the drive once the cause is fixed", NULL },
    { DR_COMM,     TAG_TYPE_DIGITAL, "Drive comm loss",  "",   0, 1,     0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and drive power", "Verify the drive address and port", NULL },
};

static const uint8_t s_drive_regs[]  = { DR_FREQ, DR_CURRENT, DR_SETPOINT, DR_TEMP };
static const uint8_t s_drive_coils[] = { DR_RUN, DR_FAULT };

static const hmi_device_action_t s_drive_actions[] = {
    { DR_RUN,      HMI_ACT_TOGGLE_COIL, HMI_CMD_PUMP_TOGGLE,  DR_FAULT,     0, 0, 0, 0,       "Stop drive", "Start drive", "Drive" },
    { DR_SETPOINT, HMI_ACT_TOGGLE_REG,  HMI_CMD_VALVE_TOGGLE, HMI_REL_NONE, 2, 400, 300, 500, "Speed to %s", "Speed to %s", "Speed setpoint" },
};

static const uint32_t s_drive_full[] = {
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_CONTAINER_CARD, 0, 0, 8, 0),
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_HERO_DIGIT,     0, 2, 8, 4),
    HMI_TOKEN_PACK(DR_CURRENT,  WIDGET_ANALOG_BAR,     0, 8, 8, 2),
    HMI_TOKEN_PACK(DR_TEMP,     WIDGET_ANALOG_BAR,     0, 11, 8, 2),
    HMI_TOKEN_PACK(DR_SETPOINT, WIDGET_CONTAINER_CARD, 8, 0, 8, 0),
    HMI_TOKEN_PACK(DR_SETPOINT, WIDGET_HERO_DIGIT,     8, 2, 8, 4),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_PUMP,           8, 6, 3, 5),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_STATUS_PILL,    11, 7, 5, 3),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_ACTION_BUTTON,  8, 12, 4, 3),
    HMI_TOKEN_PACK(DR_SETPOINT, WIDGET_ACTION_BUTTON,  12, 12, 4, 3),
};

static const uint32_t s_drive_compact[] = {
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_HERO_DIGIT,     0, 2, 10, 4),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_STATUS_PILL,    10, 3, 6, 2),
    HMI_TOKEN_PACK(DR_CURRENT,  WIDGET_ANALOG_BAR,     0, 8, 8, 2),
    HMI_TOKEN_PACK(DR_TEMP,     WIDGET_ANALOG_BAR,     8, 8, 8, 2),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_ACTION_BUTTON,  0, 12, 8, 3),
    HMI_TOKEN_PACK(DR_SETPOINT, WIDGET_ACTION_BUTTON,  8, 12, 8, 3),
};

const hmi_device_profile_t g_profile_drive = {
    "drive", "Drive", "schneider", "atv", true, true,
    s_drive_meta, (uint8_t)(sizeof(s_drive_meta) / sizeof(s_drive_meta[0])),
    { 0, (uint8_t)sizeof(s_drive_regs), s_drive_regs },
    { 0, (uint8_t)sizeof(s_drive_coils), s_drive_coils },
    s_drive_actions, (uint8_t)(sizeof(s_drive_actions) / sizeof(s_drive_actions[0])),
    DR_COMM, HMI_REL_NONE, 0xFFFFu, DR_FREQ,
    s_drive_full, (uint8_t)(sizeof(s_drive_full) / sizeof(s_drive_full[0])),
    s_drive_compact, (uint8_t)(sizeof(s_drive_compact) / sizeof(s_drive_compact[0])),
};
