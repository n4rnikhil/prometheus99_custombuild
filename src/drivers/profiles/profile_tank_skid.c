/**
 * @file profile_tank_skid.c
 * @brief Driver module: tank skid on a Schneider Modicon M340 (BMX P34 CPUs)
 *
 * A driver module is pure const data (portable C99): identification, tags, Modbus map,
 * operator controls, alarms and its own screen layouts. The core HMI knows nothing about
 * the device; the module is only used while such a device is connected.
 *
 * Map (%MWn = holding register n, %Mn = coil n):
 *   %MW0 level 0.1 %   %MW1 pump A rpm   %MW2 valve %   %MW3 pressure kPa   %MW4 coolant C
 *   %MW5 level setpoint 0.1 %   %MW6 mode (0 Manual, 1 Auto: the PLC runs pump A to hold the level)
 *   %M0 pump A   %M1 coolant pump B   %M2 E-stop   %M3 fault reset (pulse)   %M4 pump A overload
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { TK_LEVEL = 0, TK_RPM, TK_VALVE, TK_PRESS, TK_TEMP, TK_LEVEL_SP, TK_MODE,
       TK_PUMP_A, TK_PUMP_B, TK_ESTOP, TK_TRIP, TK_COMM };

static const hmi_tag_meta_t s_meta[] = {
    /* rel id      type             name              unit   min  max   warn  trip  dec dig-sev             action 1                                   action 2 */
    { TK_LEVEL,    TAG_TYPE_ANALOG,  "Tank 01 level",  "%",   0, 1000,  850,  950, 1, 0,                  "Open outlet valve V-101 to drain",        "Stop feed pump A if the level keeps rising", NULL },
    { TK_RPM,      TAG_TYPE_ANALOG,  "Feed pump A",    "RPM", 0, 3600, 3200, 3500, 0, 0,                  "Reduce the pump speed command",           "Inspect pump bearings and seals", NULL },
    { TK_VALVE,    TAG_TYPE_ANALOG,  "Valve V-101",    "%",   0,  100,    0,    0, 0, 0,                  NULL, NULL, NULL },
    { TK_PRESS,    TAG_TYPE_ANALOG,  "Line pressure",  "kPa", 0,  250,  180,  210, 0, 0,                  "Open relief valve RV-201",                "Reduce feed pump speed", NULL },
    { TK_TEMP,     TAG_TYPE_ANALOG,  "Coolant temp",   "C",   0,  120,   70,   85, 0, 0,                  "Start coolant pump B",                    "Reduce the thermal load", NULL },
    { TK_LEVEL_SP, TAG_TYPE_ANALOG,  "Level setpoint", "%",   0, 1000,    0,    0, 1, 0,                  NULL, NULL, NULL },
    { TK_MODE,     TAG_TYPE_ANALOG,  "Control mode",   "",    0,    1,    0,    0, 0, 0,                  NULL, NULL, NULL },
    { TK_PUMP_A,   TAG_TYPE_DIGITAL, "Pump A",         "",    0,    1,    0,    0, 0, 0,                  NULL, NULL, NULL },
    { TK_PUMP_B,   TAG_TYPE_DIGITAL, "Coolant pump B", "",    0,    1,    0,    0, 0, 0,                  NULL, NULL, NULL },
    { TK_ESTOP,    TAG_TYPE_DIGITAL, "Emergency stop", "",    0,    1,    0,    0, 0, ALARM_SEV_ESTOP,    "Make the plant safe before resetting",    "Acknowledge, then press Reset E-stop", NULL },
    { TK_TRIP,     TAG_TYPE_DIGITAL, "Pump A overload","",    0,    1,    0,    0, 0, ALARM_SEV_WARNING,  "Check pump A motor and its thermal relay", "Press Reset once the cause is cleared", NULL },
    { TK_COMM,     TAG_TYPE_DIGITAL, "PLC comm loss",  "",    0,    1,    0,    0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power",  "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { TK_LEVEL, TK_RPM, TK_VALVE, TK_PRESS, TK_TEMP, TK_LEVEL_SP, TK_MODE };   /* %MW0..6 */
static const uint8_t s_coils[] = { TK_PUMP_A, TK_PUMP_B, TK_ESTOP, HMI_REL_NONE, TK_TRIP };                /* %M0..4 */

static const hmi_device_action_t s_actions[] = {
    HMI_A_START(TK_PUMP_A, 0, TK_ESTOP, "Start pump", "Pump A", false),
    HMI_A_STOP (TK_PUMP_A, 0, "Stop pump", "Pump A", false),
    HMI_A_START(TK_PUMP_B, 1, TK_ESTOP, "Start", "Coolant pump B", false),
    HMI_A_STOP (TK_PUMP_B, 1, "Stop", "Coolant pump B", false),
    HMI_A_SET  (TK_VALVE, 2, HMI_CMD_SETPOINT, "Set valve", "Valve V-101", 0, 100),
    HMI_A_SET  (TK_LEVEL_SP, 5, HMI_CMD_SETPOINT, "Set level", "Level setpoint", 100, 950),
    HMI_A_MODE (TK_MODE, 6, "Manual|Auto", "Control mode", true),
    HMI_A_PULSE(TK_TRIP, 3, "Reset", "Pump A overload", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_HERO_DIGIT,     0, 2, 4, 4),
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_TANK,           4, 2, 2, 7),
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(TK_RPM,    WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(TK_RPM,    WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(TK_PRESS,  WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(TK_PRESS,  WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(TK_TEMP,   WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(TK_VALVE,  WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(TK_VALVE,  WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(TK_LEVEL, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(TK_LEVEL, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(TK_PRESS, WIDGET_ANALOG_BAR    , 10, 7, 6, 3),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(TK_VALVE, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_tank_skid = {
    "tank_skid", "Tank skid", "schneider", "bmx p34|m340", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    TK_COMM, TK_ESTOP, 2, TK_LEVEL,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
