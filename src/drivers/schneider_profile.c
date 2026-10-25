/**
 * @file schneider_profile.c
 * @brief Device profile for the Modicon M340/M580 process: tag metadata + UI manifest
 *
 * Pure data, portable C99 (no OS headers). The transport (Modbus TCP or the
 * simulator) lives in driver_modbus.c; anything that can draw the HMI - the
 * Win32 runtime, the portable snapshot tool, a microcontroller port - uses this
 * profile unchanged. The renderer draws ONLY the widgets listed in the manifest.
 */

#include "../../include/drivers/driver_modbus.h"
#include "../../include/renderer/bytecode.h"

const hmi_tag_meta_t g_schneider_meta[SCHNEIDER_TAG_COUNT] = {
    /* id                          type             name              unit   min  max   warn  trip  dec dig-sev             action 1                                  action 2 */
    { SCHNEIDER_TAG_TANK_LEVEL,    TAG_TYPE_ANALOG,  "Tank 01 level",  "%",   0, 1000,  850,  950, 1, 0,                  "Open outlet valve V-101 to drain",       "Stop feed pump A if the level keeps rising" },
    { SCHNEIDER_TAG_PUMP_RPM,      TAG_TYPE_ANALOG,  "Feed pump A",    "RPM", 0, 3600, 3200, 3500, 0, 0,                  "Reduce the pump speed command",          "Inspect pump bearings and seals" },
    { SCHNEIDER_TAG_VALVE_POS,     TAG_TYPE_ANALOG,  "Valve V-101",    "%",   0,  100,    0,    0, 0, 0,                  NULL,                                     NULL },
    { SCHNEIDER_TAG_REACTOR_PRESS, TAG_TYPE_ANALOG,  "Line pressure",  "kPa", 0,  250,  180,  210, 0, 0,                  "Open relief valve RV-201",               "Reduce feed pump speed" },
    { SCHNEIDER_TAG_COOLANT_TEMP,  TAG_TYPE_ANALOG,  "Coolant temp",   "C",   0,  120,   70,   85, 0, 0,                  "Start coolant pump B",                   "Reduce the thermal load" },
    { SCHNEIDER_TAG_PUMP_RUNNING,  TAG_TYPE_DIGITAL, "Pump A",         "",    0,    1,    0,    0, 0, 0,                  NULL,                                     NULL },
    { SCHNEIDER_TAG_PUMP_B_RUN,    TAG_TYPE_DIGITAL, "Coolant pump B", "",    0,    1,    0,    0, 0, 0,                  NULL,                                     NULL },
    { SCHNEIDER_TAG_ESTOP_ACTIVE,  TAG_TYPE_DIGITAL, "Emergency stop", "",    0,    1,    0,    0, 0, ALARM_SEV_ESTOP,    "Make the plant safe before resetting",   "Acknowledge, then press Reset E-stop" },
    { SCHNEIDER_TAG_COMM_LOSS,     TAG_TYPE_DIGITAL, "PLC comm loss",  "",    0,    1,    0,    0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port" },
};

/* 14 tokens = 56 bytes: the entire process screen */
const uint32_t g_schneider_manifest[] = {
    /* Tank 01 */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL,    WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL,    WIDGET_HERO_DIGIT,     0, 2, 4, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL,    WIDGET_TANK,           4, 2, 2, 7),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_TANK_LEVEL,    WIDGET_TREND_SPARK,    0, 10, 6, 5),
    /* Feed pump A */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RPM,      WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RPM,      WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RUNNING,  WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RUNNING,  WIDGET_STATUS_PILL,    8, 7, 3, 3),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_PUMP_RUNNING,  WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    /* Reactor line */
    HMI_TOKEN_PACK(SCHNEIDER_TAG_REACTOR_PRESS, WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_REACTOR_PRESS, WIDGET_HERO_DIGIT,     11, 2, 5, 4),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_COOLANT_TEMP,  WIDGET_ANALOG_BAR,     11, 7, 5, 2),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_VALVE_POS,     WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(SCHNEIDER_TAG_VALVE_POS,     WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};
