/**
 * @file profile_drive.c
 * @brief Driver module: Schneider Altivar ATV320 variable-speed drive
 *
 * Map (0-based): HR0 output 0.1 Hz, HR1 current 0.1 A, HR2 speed setpoint 0.1 Hz,
 * HR3 drive temperature C, HR4 acceleration time 0.1 s, HR5 direction (0 fwd, 1 rev),
 * HR6 motor power 0.1 kW; coil0 run, coil1 fault, coil2 fault reset (pulse).
 * This is the layout of the PLC Lab drive; a real Altivar uses its own parameter
 * addresses (RFR 3202, LCR 3204, CMD 8501, LFR 8502 ...), which a site module would carry.
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { DR_FREQ = 0, DR_CURRENT, DR_SPEED_SP, DR_TEMP, DR_ACCEL, DR_DIR, DR_POWER, DR_RUN, DR_FAULT, DR_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { DR_FREQ,     TAG_TYPE_ANALOG,  "Output frequency", "Hz", 0, 600,   0,   0, 1, 0, NULL, NULL, NULL },
    { DR_CURRENT,  TAG_TYPE_ANALOG,  "Motor current",    "A",  0, 400, 300, 360, 1, 0, "Check the driven load for jams", "Reduce the speed setpoint", NULL },
    { DR_SPEED_SP, TAG_TYPE_ANALOG,  "Speed setpoint",   "Hz", 0, 600,   0,   0, 1, 0, NULL, NULL, NULL },
    { DR_TEMP,     TAG_TYPE_ANALOG,  "Drive temp",       "C",  0, 120,  70,  85, 0, 0, "Check the cabinet fan and filter", "Reduce the load or the speed", NULL },
    { DR_ACCEL,    TAG_TYPE_ANALOG,  "Accel time",       "s",  0, 600,   0,   0, 1, 0, NULL, NULL, NULL },
    { DR_DIR,      TAG_TYPE_ANALOG,  "Direction",        "",   0,   1,   0,   0, 0, 0, NULL, NULL, NULL },
    { DR_POWER,    TAG_TYPE_ANALOG,  "Motor power",      "kW", 0, 500,   0,   0, 1, 0, NULL, NULL, NULL },
    { DR_RUN,      TAG_TYPE_DIGITAL, "Drive",            "",   0,   1,   0,   0, 0, 0, NULL, NULL, NULL },
    { DR_FAULT,    TAG_TYPE_DIGITAL, "Drive fault",      "",   0,   1,   0,   0, 0, ALARM_SEV_CRITICAL, "Read the fault code on the drive display", "Press Reset fault once the cause is cleared", NULL },
    { DR_COMM,     TAG_TYPE_DIGITAL, "Drive comm loss",  "",   0,   1,   0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and drive power", "Verify the drive address and port", NULL },
};

static const uint8_t s_regs[]  = { DR_FREQ, DR_CURRENT, DR_SPEED_SP, DR_TEMP, DR_ACCEL, DR_DIR, DR_POWER };
static const uint8_t s_coils[] = { DR_RUN, DR_FAULT };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(DR_RUN, 0, DR_FAULT, "Start drive", "Drive", false),
    HMI_A_STOP (DR_RUN, 0, "Stop drive", "Drive", false),
    HMI_A_SET  (DR_SPEED_SP, 2, HMI_CMD_SETPOINT, "Set speed", "Speed setpoint", 0, 600),
    HMI_A_MODE (DR_DIR, 5, "Forward|Reverse", "Direction", true),
    HMI_A_SET  (DR_ACCEL, 4, HMI_CMD_TUNE, "Set ramp", "Accel time", 1, 600),
    HMI_A_PULSE(DR_FAULT, 2, "Reset fault", "Drive fault", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(DR_SPEED_SP, WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(DR_FREQ,     WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(DR_CURRENT,  WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(DR_CURRENT,  WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(DR_RUN,      WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(DR_TEMP,     WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(DR_TEMP,     WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(DR_POWER,    WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(DR_ACCEL,    WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(DR_SPEED_SP, WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(DR_FREQ, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(DR_FREQ, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(DR_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(DR_CURRENT, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(DR_RUN, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(DR_SPEED_SP, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_drive = {
    "drive", "Drive", "schneider", "atv", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    DR_COMM, HMI_REL_NONE, 0xFFFFu, DR_FREQ,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
