/**
 * @file profile_compressor.c
 * @brief Driver module: air compressor on an Allen-Bradley Micro850 (Modbus TCP server)
 *
 * Map: HR0 air pressure 0.01 bar, HR1 load (cut-in) pressure, HR2 unload (cut-out) pressure,
 * HR3 discharge temp 0.1 C, HR4 motor current 0.1 A, HR5 state (0 stopped, 1 loaded,
 * 2 unloaded), HR6 air demand %; coil0 run, coil1 high-temperature trip, coil2 trip reset (pulse).
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { CP_PRESS = 0, CP_LOAD, CP_UNLOAD, CP_TEMP, CP_CURRENT, CP_RUN, CP_TRIP, CP_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { CP_PRESS,   TAG_TYPE_ANALOG,  "Air pressure",    "bar", 0, 1200, 950, 1050, 2, 0, "Check the unload valve", "Lower the unload pressure", NULL },
    { CP_LOAD,    TAG_TYPE_ANALOG,  "Load pressure",   "bar", 0, 1200,   0,    0, 2, 0, NULL, NULL, NULL },
    { CP_UNLOAD,  TAG_TYPE_ANALOG,  "Unload pressure", "bar", 0, 1200,   0,    0, 2, 0, NULL, NULL, NULL },
    { CP_TEMP,    TAG_TYPE_ANALOG,  "Discharge temp",  "C",   0, 1500, 1000, 1080, 1, 0, "Check the oil level and the cooler", "Clean the cooler fins", NULL },
    { CP_CURRENT, TAG_TYPE_ANALOG,  "Motor current",   "A",   0, 1000,   0,    0, 1, 0, NULL, NULL, NULL },
    { CP_RUN,     TAG_TYPE_DIGITAL, "Compressor",      "",    0, 1,      0,    0, 0, 0, NULL, NULL, NULL },
    { CP_TRIP,    TAG_TYPE_DIGITAL, "High temp trip",  "",    0, 1,      0,    0, 0, ALARM_SEV_CRITICAL, "Let the compressor cool down; check oil and cooler", "Reset the trip below 100 C, then Start", NULL },
    { CP_COMM,    TAG_TYPE_DIGITAL, "PLC comm loss",   "",    0, 1,      0,    0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { CP_PRESS, CP_LOAD, CP_UNLOAD, CP_TEMP, CP_CURRENT };
static const uint8_t s_coils[] = { CP_RUN, CP_TRIP };

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(CP_RUN, 0, CP_TRIP, "Start", "Compressor", false),
    HMI_A_STOP (CP_RUN, 0, "Stop", "Compressor", true),
    HMI_A_SET  (CP_LOAD, 1, HMI_CMD_SETPOINT, "Set", "Load pressure", 400, 900),
    HMI_A_SET  (CP_UNLOAD, 2, HMI_CMD_SETPOINT, "Set", "Unload pressure", 450, 1000),
    HMI_A_PULSE(CP_TRIP, 2, "Reset trip", "High temp trip", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(CP_PRESS,   WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(CP_PRESS,   WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(CP_UNLOAD,  WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(CP_PRESS,   WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(CP_CURRENT, WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(CP_CURRENT, WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(CP_RUN,     WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(CP_RUN,     WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(CP_RUN,     WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(CP_TEMP,    WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(CP_TEMP,    WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(CP_LOAD,    WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(CP_LOAD,    WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(CP_PRESS, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(CP_PRESS, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(CP_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(CP_TEMP, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(CP_RUN, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(CP_LOAD, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_compressor = {
    "compressor", "Compressor", "rockwell|allen", "2080-lc|micro850", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    CP_COMM, HMI_REL_NONE, 0xFFFFu, CP_PRESS,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
