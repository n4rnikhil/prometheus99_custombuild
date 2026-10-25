/**
 * @file profile_boiler.c
 * @brief Driver module: hot-water boiler on a Schneider Modicon M580 (BME P58 CPUs)
 *
 * Map: %MW0 water temp 0.1 C, %MW1 temp setpoint 0.1 C, %MW2 firing rate %, %MW3 pressure kPa,
 * %MW4 drum level %, %MW5 mode (0 Manual, 1 Auto: PLC PID on temperature), %MW6 manual firing %;
 * %M0 burner enable, %M1 flame, %M2 low-water trip, %M3 E-stop, %M4 trip reset (pulse).
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { BL_TEMP = 0, BL_SP, BL_FIRE, BL_PRESS, BL_LEVEL, BL_MODE, BL_MAN, BL_BURNER, BL_FLAME, BL_LOWW, BL_ESTOP, BL_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { BL_TEMP,   TAG_TYPE_ANALOG,  "Water temp",      "C",   0, 1200, 900, 950, 1, 0, "Lower the firing rate or the setpoint", "Check circulation pumps and valves", NULL },
    { BL_SP,     TAG_TYPE_ANALOG,  "Temp setpoint",   "C",   0, 1000,   0,   0, 1, 0, NULL, NULL, NULL },
    { BL_FIRE,   TAG_TYPE_ANALOG,  "Firing rate",     "%",   0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { BL_PRESS,  TAG_TYPE_ANALOG,  "Boiler pressure", "kPa", 0, 600,  400, 450, 0, 0, "Reduce the firing rate", "Check the safety valve", NULL },
    { BL_LEVEL,  TAG_TYPE_ANALOG,  "Drum level",      "%",   0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { BL_MODE,   TAG_TYPE_ANALOG,  "Control mode",    "",    0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { BL_MAN,    TAG_TYPE_ANALOG,  "Manual firing",   "%",   0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { BL_BURNER, TAG_TYPE_DIGITAL, "Burner",          "",    0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { BL_FLAME,  TAG_TYPE_DIGITAL, "Flame",           "",    0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { BL_LOWW,   TAG_TYPE_DIGITAL, "Low water trip",  "",    0, 1,      0,   0, 0, ALARM_SEV_CRITICAL, "Check the feed-water pump and the drum level", "Reset the trip once the level is above 20 %", NULL },
    { BL_ESTOP,  TAG_TYPE_DIGITAL, "Emergency stop",  "",    0, 1,      0,   0, 0, ALARM_SEV_ESTOP, "Make the boiler house safe before resetting", "Acknowledge, then press Reset E-stop", NULL },
    { BL_COMM,   TAG_TYPE_DIGITAL, "PLC comm loss",   "",    0, 1,      0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { BL_TEMP, BL_SP, BL_FIRE, BL_PRESS, BL_LEVEL, BL_MODE, BL_MAN };
static const uint8_t s_coils[] = { BL_BURNER, BL_FLAME, BL_LOWW, BL_ESTOP };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(BL_BURNER, 0, BL_ESTOP, "Light burner", "Burner", true),
    HMI_A_STOP (BL_BURNER, 0, "Stop burner", "Burner", false),
    HMI_A_MODE (BL_MODE, 5, "Manual|Auto", "Control mode", true),
    HMI_A_SET  (BL_SP, 1, HMI_CMD_SETPOINT, "Set temp", "Temperature setpoint", 200, 950),
    HMI_A_SET  (BL_MAN, 6, HMI_CMD_SETPOINT, "Set firing", "Manual firing rate", 0, 100),
    HMI_A_PULSE(BL_LOWW, 4, "Reset trip", "Low water trip", true),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(BL_TEMP,   WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(BL_TEMP,   WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(BL_SP,     WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(BL_TEMP,   WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(BL_FIRE,   WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(BL_FIRE,   WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(BL_FLAME,  WIDGET_STATUS_PILL,    6, 7, 5, 3),
    HMI_TOKEN_PACK(BL_BURNER, WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(BL_PRESS,  WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(BL_PRESS,  WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(BL_LEVEL,  WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(BL_MAN,    WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(BL_SP,     WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(BL_TEMP, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(BL_TEMP, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(BL_FLAME, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(BL_FIRE, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(BL_BURNER, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(BL_SP, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_boiler = {
    "boiler", "Boiler", "schneider", "bme p58|m580", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    BL_COMM, BL_ESTOP, 3, BL_TEMP,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
