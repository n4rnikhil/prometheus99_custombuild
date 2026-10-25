/**
 * @file profile_tank_skid.c
 * @brief Driver module: Schneider Modicon M340 / M580 tank skid (BMX / BME CPUs)
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
/* Tank skid  (Schneider Modicon M340 / M580 running the tank application)    */
/* ========================================================================= */
enum { TK_LEVEL = 0, TK_RPM, TK_VALVE, TK_PRESS, TK_TEMP, TK_PUMP_A, TK_PUMP_B, TK_ESTOP, TK_COMM };

static const hmi_tag_meta_t s_tank_meta[] = {
    /* rel id   type             name              unit   min  max   warn  trip  dec dig-sev             action 1                                   action 2 */
    { TK_LEVEL,  TAG_TYPE_ANALOG,  "Tank 01 level",  "%",   0, 1000,  850,  950, 1, 0,                  "Open outlet valve V-101 to drain",        "Stop feed pump A if the level keeps rising", NULL },
    { TK_RPM,    TAG_TYPE_ANALOG,  "Feed pump A",    "RPM", 0, 3600, 3200, 3500, 0, 0,                  "Reduce the pump speed command",           "Inspect pump bearings and seals", NULL },
    { TK_VALVE,  TAG_TYPE_ANALOG,  "Valve V-101",    "%",   0,  100,    0,    0, 0, 0,                  NULL,                                      NULL, NULL },
    { TK_PRESS,  TAG_TYPE_ANALOG,  "Line pressure",  "kPa", 0,  250,  180,  210, 0, 0,                  "Open relief valve RV-201",                "Reduce feed pump speed", NULL },
    { TK_TEMP,   TAG_TYPE_ANALOG,  "Coolant temp",   "C",   0,  120,   70,   85, 0, 0,                  "Start coolant pump B",                    "Reduce the thermal load", NULL },
    { TK_PUMP_A, TAG_TYPE_DIGITAL, "Pump A",         "",    0,    1,    0,    0, 0, 0,                  NULL,                                      NULL, NULL },
    { TK_PUMP_B, TAG_TYPE_DIGITAL, "Coolant pump B", "",    0,    1,    0,    0, 0, 0,                  NULL,                                      NULL, NULL },
    { TK_ESTOP,  TAG_TYPE_DIGITAL, "Emergency stop", "",    0,    1,    0,    0, 0, ALARM_SEV_ESTOP,    "Make the plant safe before resetting",    "Acknowledge, then press Reset E-stop", NULL },
    { TK_COMM,   TAG_TYPE_DIGITAL, "PLC comm loss",  "",    0,    1,    0,    0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power",  "Verify the PLC address and port", NULL },
};

static const uint8_t s_tank_regs[]  = { TK_LEVEL, TK_RPM, TK_VALVE, TK_PRESS, TK_TEMP };   /* %MW0..%MW4 */
static const uint8_t s_tank_coils[] = { TK_PUMP_A, TK_PUMP_B, TK_ESTOP };                  /* %M0..%M2   */

static const hmi_device_action_t s_tank_actions[] = {
    { TK_PUMP_A, HMI_ACT_TOGGLE_COIL, HMI_CMD_PUMP_TOGGLE,  TK_ESTOP,     0, 0, 0, 0,   "Stop pump", "Start pump", "Pump A" },
    { TK_VALVE,  HMI_ACT_TOGGLE_REG,  HMI_CMD_VALVE_TOGGLE, HMI_REL_NONE, 2, 50, 25, 85, "Move valve to %s", "Move valve to %s", "Valve V-101" },
};

static const uint32_t s_tank_full[] = {
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

static const uint32_t s_tank_compact[] = {
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_HERO_DIGIT,     0, 2, 10, 4),
    HMI_TOKEN_PACK(TK_LEVEL,  WIDGET_TANK,           11, 2, 5, 7),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_STATUS_PILL,    1, 6, 7, 2),
    HMI_TOKEN_PACK(TK_PRESS,  WIDGET_ANALOG_BAR,     0, 9, 8, 2),
    HMI_TOKEN_PACK(TK_TEMP,   WIDGET_ANALOG_BAR,     8, 9, 8, 2),
    HMI_TOKEN_PACK(TK_PUMP_A, WIDGET_ACTION_BUTTON,  0, 12, 8, 3),
    HMI_TOKEN_PACK(TK_VALVE,  WIDGET_ACTION_BUTTON,  8, 12, 8, 3),
};

const hmi_device_profile_t g_profile_tank_skid = {
    "tank_skid", "Tank skid", "schneider", "bmx|bme|m340|m580", true, true,
    s_tank_meta, (uint8_t)(sizeof(s_tank_meta) / sizeof(s_tank_meta[0])),
    { 0, (uint8_t)sizeof(s_tank_regs), s_tank_regs },
    { 0, (uint8_t)sizeof(s_tank_coils), s_tank_coils },
    s_tank_actions, (uint8_t)(sizeof(s_tank_actions) / sizeof(s_tank_actions[0])),
    TK_COMM, TK_ESTOP, 2, TK_LEVEL,
    s_tank_full, (uint8_t)(sizeof(s_tank_full) / sizeof(s_tank_full[0])),
    s_tank_compact, (uint8_t)(sizeof(s_tank_compact) / sizeof(s_tank_compact[0])),
};
