/**
 * @file profile_ahu.c
 * @brief Driver module: air handling unit on a Schneider Modicon M251 (TM251 controllers)
 *
 * Map: %MW0 supply air 0.1 C, %MW1 supply setpoint 0.1 C, %MW2 return air 0.1 C, %MW3 fan
 * speed %, %MW4 fan speed setpoint %, %MW5 mode (0 Off, 1 Heat, 2 Cool, 3 Auto), %MW6 fresh-air
 * damper %, %MW7 filter pressure drop Pa, %MW8 CO2 ppm; %M0 supply fan, %M1 filter dirty,
 * %M2 fire alarm (stops the fan).
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { AH_SUPPLY = 0, AH_SP, AH_RETURN, AH_FAN, AH_FAN_SP, AH_MODE, AH_DAMPER, AH_FILTER, AH_CO2,
       AH_FAN_RUN, AH_DIRTY, AH_FIRE, AH_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { AH_SUPPLY,  TAG_TYPE_ANALOG,  "Supply air",       "C",   -100, 500, 320, 350, 1, 0, "Check the heating valve", "Lower the supply setpoint", NULL },
    { AH_SP,      TAG_TYPE_ANALOG,  "Supply setpoint",  "C",   -100, 500,   0,   0, 1, 0, NULL, NULL, NULL },
    { AH_RETURN,  TAG_TYPE_ANALOG,  "Return air",       "C",   -100, 500,   0,   0, 1, 0, NULL, NULL, NULL },
    { AH_FAN,     TAG_TYPE_ANALOG,  "Fan speed",        "%",   0,    100,   0,   0, 0, 0, NULL, NULL, NULL },
    { AH_FAN_SP,  TAG_TYPE_ANALOG,  "Fan setpoint",     "%",   0,    100,   0,   0, 0, 0, NULL, NULL, NULL },
    { AH_MODE,    TAG_TYPE_ANALOG,  "Mode",             "",    0,    3,     0,   0, 0, 0, NULL, NULL, NULL },
    { AH_DAMPER,  TAG_TYPE_ANALOG,  "Fresh-air damper", "%",   0,    100,   0,   0, 0, 0, NULL, NULL, NULL },
    { AH_FILTER,  TAG_TYPE_ANALOG,  "Filter pressure",  "Pa",  0,    500, 250, 400, 0, 0, "Plan a filter change", "Change the filter now", NULL },
    { AH_CO2,     TAG_TYPE_ANALOG,  "CO2",              "ppm", 0,   3000, 1000, 1500, 0, 0, "Raise the fan speed setpoint", "Check the fresh-air damper", NULL },
    { AH_FAN_RUN, TAG_TYPE_DIGITAL, "Supply fan",       "",    0,    1,     0,   0, 0, 0, NULL, NULL, NULL },
    { AH_DIRTY,   TAG_TYPE_DIGITAL, "Filter dirty",     "",    0,    1,     0,   0, 0, ALARM_SEV_WARNING, "Change the filter", "The fan uses more energy until then", NULL },
    { AH_FIRE,    TAG_TYPE_DIGITAL, "Fire alarm",       "",    0,    1,     0,   0, 0, ALARM_SEV_CRITICAL, "Fan stopped by the fire panel", "Restart only after the fire panel is reset", NULL },
    { AH_COMM,    TAG_TYPE_DIGITAL, "PLC comm loss",    "",    0,    1,     0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { AH_SUPPLY, AH_SP, AH_RETURN, AH_FAN, AH_FAN_SP, AH_MODE, AH_DAMPER, AH_FILTER, AH_CO2 };
static const uint8_t s_coils[] = { AH_FAN_RUN, AH_DIRTY, AH_FIRE };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(AH_FAN_RUN, 0, AH_FIRE, "Start fan", "Supply fan", false),
    HMI_A_STOP (AH_FAN_RUN, 0, "Stop fan", "Supply fan", false),
    HMI_A_MODE (AH_MODE, 5, "Off|Heat|Cool|Auto", "Unit mode", false),
    HMI_A_SET  (AH_SP, 1, HMI_CMD_SETPOINT, "Set temp", "Supply setpoint", 120, 300),
    HMI_A_SET  (AH_FAN_SP, 4, HMI_CMD_SETPOINT, "Set fan", "Fan speed setpoint", 20, 100),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(AH_SUPPLY,  WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(AH_SUPPLY,  WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(AH_SP,      WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(AH_SUPPLY,  WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(AH_FAN,     WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(AH_FAN,     WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(AH_FAN_RUN, WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(AH_FAN_RUN, WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(AH_FAN_RUN, WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(AH_CO2,     WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(AH_CO2,     WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(AH_DAMPER,  WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(AH_FILTER,  WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(AH_MODE,    WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(AH_SUPPLY, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(AH_SUPPLY, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(AH_FAN_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(AH_CO2, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(AH_FAN_RUN, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(AH_MODE, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_ahu = {
    "ahu", "Air handler", "schneider", "tm251|m251", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    AH_COMM, HMI_REL_NONE, 0xFFFFu, AH_SUPPLY,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
