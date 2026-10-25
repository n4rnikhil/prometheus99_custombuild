/**
 * @file profile_abb_drive.c
 * @brief Driver module: ABB ACS580 drive (Modbus TCP adapter)
 *
 * Map (PLC Lab layout): HR0 motor speed rpm, HR1 speed reference rpm, HR2 current 0.1 A,
 * HR3 torque %, HR4 DC bus V, HR5 fault code; coil0 run, coil1 fault, coil2 fault reset (pulse).
 * A real ACS580 exposes its control / status words and parameters through its own Modbus map
 * (control word, status word, reference 1, actual 1 ...), which a site module would carry.
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { AB_SPEED = 0, AB_REF, AB_CURRENT, AB_TORQUE, AB_DC, AB_FCODE, AB_RUN, AB_FAULT, AB_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { AB_SPEED,   TAG_TYPE_ANALOG,  "Motor speed",     "rpm", 0, 1800, 1600, 1700, 0, 0, "Check the speed reference", "Check the encoder / motor data", NULL },
    { AB_REF,     TAG_TYPE_ANALOG,  "Speed reference", "rpm", 0, 1800,    0,    0, 0, 0, NULL, NULL, NULL },
    { AB_CURRENT, TAG_TYPE_ANALOG,  "Motor current",   "A",   0, 500,   350,  420, 1, 0, "Check the driven load", "Reduce the speed reference", NULL },
    { AB_TORQUE,  TAG_TYPE_ANALOG,  "Torque",          "%",   0, 200,   150,  180, 0, 0, "Check the driven load", NULL, NULL },
    { AB_DC,      TAG_TYPE_ANALOG,  "DC bus",          "V",   0, 900,     0,    0, 0, 0, NULL, NULL, NULL },
    { AB_FCODE,   TAG_TYPE_ANALOG,  "Fault code",      "",    0, 32000,   0,    0, 0, 0, NULL, NULL, NULL },
    { AB_RUN,     TAG_TYPE_DIGITAL, "Drive",           "",    0, 1,       0,    0, 0, 0, NULL, NULL, NULL },
    { AB_FAULT,   TAG_TYPE_DIGITAL, "Drive fault",     "",    0, 1,       0,    0, 0, ALARM_SEV_CRITICAL, "Read the fault code (2310 = overcurrent)", "Press Reset fault once the cause is cleared", NULL },
    { AB_COMM,    TAG_TYPE_DIGITAL, "Drive comm loss", "",    0, 1,       0,    0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and drive power", "Verify the drive address and port", NULL },
};

static const uint8_t s_regs[]  = { AB_SPEED, AB_REF, AB_CURRENT, AB_TORQUE, AB_DC, AB_FCODE };
static const uint8_t s_coils[] = { AB_RUN, AB_FAULT };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(AB_RUN, 0, AB_FAULT, "Start drive", "Drive", false),
    HMI_A_STOP (AB_RUN, 0, "Stop drive", "Drive", false),
    HMI_A_SET  (AB_REF, 1, HMI_CMD_SETPOINT, "Set speed", "Speed reference", 0, 1500),
    HMI_A_PULSE(AB_FAULT, 2, "Reset fault", "Drive fault", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(AB_SPEED,   WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(AB_SPEED,   WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(AB_REF,     WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(AB_SPEED,   WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(AB_CURRENT, WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(AB_CURRENT, WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(AB_RUN,     WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(AB_RUN,     WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(AB_RUN,     WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(AB_TORQUE,  WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(AB_TORQUE,  WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(AB_DC,      WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(AB_REF,     WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(AB_SPEED, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(AB_SPEED, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(AB_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(AB_CURRENT, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(AB_RUN, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(AB_REF, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_abb_drive = {
    "abb_drive", "ABB drive", "abb", "acs580|acs880|acs380", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    AB_COMM, HMI_REL_NONE, 0xFFFFu, AB_SPEED,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
