/**
 * @file profile_conveyor.c
 * @brief Driver module: conveyor line on a Schneider Modicon M241 (TM241 controllers)
 *
 * Map: %MW0 belt speed 0.1 m/min, %MW1 speed setpoint 0.1 m/min, %MW2 item count,
 * %MW3 motor current 0.1 A, %MW4 throughput items/min; %M0 run, %M1 jam detected,
 * %M2 pull-cord stop (E-stop), %M3 jam reset (pulse), %M4 counter reset (pulse).
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { CV_SPEED = 0, CV_SP, CV_COUNT, CV_CURRENT, CV_RATE, CV_RUN, CV_JAM, CV_CORD, CV_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { CV_SPEED,   TAG_TYPE_ANALOG,  "Belt speed",     "m/min", 0, 1500,   0,   0, 1, 0, NULL, NULL, NULL },
    { CV_SP,      TAG_TYPE_ANALOG,  "Speed setpoint", "m/min", 0, 1200,   0,   0, 1, 0, NULL, NULL, NULL },
    { CV_COUNT,   TAG_TYPE_ANALOG,  "Item count",     "",      0, 32000,  0,   0, 0, 0, NULL, NULL, NULL },
    { CV_CURRENT, TAG_TYPE_ANALOG,  "Motor current",  "A",     0, 300,  200, 250, 1, 0, "Check the belt for drag or a stuck roller", "Reduce the belt speed", NULL },
    { CV_RATE,    TAG_TYPE_ANALOG,  "Throughput",     "/min",  0, 200,    0,   0, 0, 0, NULL, NULL, NULL },
    { CV_RUN,     TAG_TYPE_DIGITAL, "Conveyor",       "",      0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { CV_JAM,     TAG_TYPE_DIGITAL, "Jam detected",   "",      0, 1,      0,   0, 0, ALARM_SEV_WARNING, "Clear the jam at the transfer point", "Press Reset jam, then Start", NULL },
    { CV_CORD,    TAG_TYPE_DIGITAL, "Pull-cord stop", "",      0, 1,      0,   0, 0, ALARM_SEV_ESTOP,   "Find out who pulled the cord and why", "Re-arm the cord, acknowledge, then Reset E-stop", NULL },
    { CV_COMM,    TAG_TYPE_DIGITAL, "PLC comm loss",  "",      0, 1,      0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { CV_SPEED, CV_SP, CV_COUNT, CV_CURRENT, CV_RATE };
static const uint8_t s_coils[] = { CV_RUN, CV_JAM, CV_CORD };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(CV_RUN, 0, CV_CORD, "Start belt", "Conveyor", false),
    HMI_A_STOP (CV_RUN, 0, "Stop belt", "Conveyor", false),
    HMI_A_SET  (CV_SP, 1, HMI_CMD_SETPOINT, "Set speed", "Belt speed setpoint", 0, 1200),
    HMI_A_PULSE(CV_JAM, 3, "Reset jam", "Jam", false),
    HMI_A_PULSE(CV_COUNT, 4, "Reset count", "Item counter", true),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(CV_SPEED,   WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(CV_SPEED,   WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(CV_SP,      WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(CV_SPEED,   WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(CV_COUNT,   WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(CV_COUNT,   WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(CV_RUN,     WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(CV_RUN,     WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(CV_RUN,     WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(CV_RATE,    WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(CV_RATE,    WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(CV_CURRENT, WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(CV_SP,      WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(CV_SPEED, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(CV_SPEED, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(CV_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(CV_RATE, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(CV_RUN, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(CV_SP, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_conveyor = {
    "conveyor", "Conveyor", "schneider", "tm241|m241", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    CV_COMM, CV_CORD, 2, CV_SPEED,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
