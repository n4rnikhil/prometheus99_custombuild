/**
 * @file profile_pump_station.c
 * @brief Driver module: duty / standby booster pump station on a Siemens S7-1200 (Modbus TCP server)
 *
 * Map (MB_SERVER holding registers / coils): HR0 discharge pressure 0.01 bar, HR1 pressure
 * setpoint 0.01 bar, HR2 flow 0.1 m3/h, HR3 / HR4 pump 1 / 2 speed %, HR5 mode (0 Off, 1 Hand,
 * 2 Auto), HR6 hand speed %, HR7 duty pump (0 = pump 1, 1 = pump 2), HR8 demand %;
 * coil0 / coil1 pump 1 / 2 running, coil2 / coil3 pump 1 / 2 fault, coil4 fault reset (pulse).
 * In Auto the PLC runs the duty pump to hold the pressure setpoint; on a duty pump fault the
 * standby pump takes over.
 */

#include "../../../include/drivers/device_profile.h"
#include "../../../include/core/security.h"
#include "../../../include/renderer/bytecode.h"

#define N(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))

enum { PS_PRESS = 0, PS_SP, PS_FLOW, PS_P1, PS_P2, PS_MODE, PS_HAND, PS_DUTY, PS_DEMAND,
       PS_P1_RUN, PS_P2_RUN, PS_P1_FLT, PS_P2_FLT, PS_COMM };

static const hmi_tag_meta_t s_meta[] = {
    { PS_PRESS,  TAG_TYPE_ANALOG,  "Discharge pressure", "bar",  0, 1000, 700, 800, 2, 0, "Check the pressure transmitter", "Lower the pressure setpoint", NULL },
    { PS_SP,     TAG_TYPE_ANALOG,  "Pressure setpoint",  "bar",  0, 1000,   0,   0, 2, 0, NULL, NULL, NULL },
    { PS_FLOW,   TAG_TYPE_ANALOG,  "Flow",               "m3/h", 0, 2000,   0,   0, 1, 0, NULL, NULL, NULL },
    { PS_P1,     TAG_TYPE_ANALOG,  "Pump 1 speed",       "%",    0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { PS_P2,     TAG_TYPE_ANALOG,  "Pump 2 speed",       "%",    0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { PS_MODE,   TAG_TYPE_ANALOG,  "Mode",               "",     0, 2,      0,   0, 0, 0, NULL, NULL, NULL },
    { PS_HAND,   TAG_TYPE_ANALOG,  "Hand speed",         "%",    0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { PS_DUTY,   TAG_TYPE_ANALOG,  "Duty pump",          "",     0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { PS_DEMAND, TAG_TYPE_ANALOG,  "Water demand",       "%",    0, 100,    0,   0, 0, 0, NULL, NULL, NULL },
    { PS_P1_RUN, TAG_TYPE_DIGITAL, "Pump 1",             "",     0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { PS_P2_RUN, TAG_TYPE_DIGITAL, "Pump 2",             "",     0, 1,      0,   0, 0, 0, NULL, NULL, NULL },
    { PS_P1_FLT, TAG_TYPE_DIGITAL, "Pump 1 fault",       "",     0, 1,      0,   0, 0, ALARM_SEV_WARNING, "Pump 2 has taken over; inspect pump 1", "Press Reset faults when repaired", NULL },
    { PS_P2_FLT, TAG_TYPE_DIGITAL, "Pump 2 fault",       "",     0, 1,      0,   0, 0, ALARM_SEV_WARNING, "Pump 1 has taken over; inspect pump 2", "Press Reset faults when repaired", NULL },
    { PS_COMM,   TAG_TYPE_DIGITAL, "PLC comm loss",      "",     0, 1,      0,   0, 0, ALARM_SEV_CRITICAL, "Check the Ethernet cable and PLC power", "Verify the PLC address and port", NULL },
};

static const uint8_t s_regs[]  = { PS_PRESS, PS_SP, PS_FLOW, PS_P1, PS_P2, PS_MODE, PS_HAND, PS_DUTY, PS_DEMAND };
static const uint8_t s_coils[] = { PS_P1_RUN, PS_P2_RUN, PS_P1_FLT, PS_P2_FLT };

static const hmi_device_action_t s_actions[] = {
    HMI_A_MODE (PS_MODE, 5, "Off|Hand|Auto", "Station mode", true),
    HMI_A_SET  (PS_SP, 1, HMI_CMD_SETPOINT, "Set pressure", "Pressure setpoint", 100, 800),
    HMI_A_SET  (PS_HAND, 6, HMI_CMD_SETPOINT, "Set speed", "Hand speed", 0, 100),
    HMI_A_MODE (PS_DUTY, 7, "Pump 1|Pump 2", "Duty pump", true),
    HMI_A_PULSE(PS_P1_FLT, 4, "Reset faults", "Pump faults", false),
};

static const uint32_t s_full[] = {
    HMI_TOKEN_PACK(PS_PRESS,  WIDGET_CONTAINER_CARD, 0, 0, 6, 0),
    HMI_TOKEN_PACK(PS_PRESS,  WIDGET_HERO_DIGIT,     0, 2, 6, 4),
    HMI_TOKEN_PACK(PS_SP,     WIDGET_ANALOG_BAR,     0, 7, 6, 2),
    HMI_TOKEN_PACK(PS_PRESS,  WIDGET_TREND_SPARK,    0, 10, 6, 5),
    HMI_TOKEN_PACK(PS_FLOW,   WIDGET_CONTAINER_CARD, 6, 0, 5, 0),
    HMI_TOKEN_PACK(PS_FLOW,   WIDGET_HERO_DIGIT,     6, 2, 5, 4),
    HMI_TOKEN_PACK(PS_P1_RUN, WIDGET_PUMP,           6, 6, 2, 5),
    HMI_TOKEN_PACK(PS_P2_RUN, WIDGET_PUMP,           8, 6, 2, 5),
    HMI_TOKEN_PACK(PS_MODE,   WIDGET_ACTION_BUTTON,  6, 12, 5, 3),
    HMI_TOKEN_PACK(PS_P1,     WIDGET_CONTAINER_CARD, 11, 0, 5, 0),
    HMI_TOKEN_PACK(PS_P1,     WIDGET_ANALOG_BAR,     11, 3, 5, 2),
    HMI_TOKEN_PACK(PS_P2,     WIDGET_ANALOG_BAR,     11, 6, 5, 2),
    HMI_TOKEN_PACK(PS_DEMAND, WIDGET_ANALOG_BAR,     11, 9, 5, 2),
    HMI_TOKEN_PACK(PS_SP,     WIDGET_ACTION_BUTTON,  11, 12, 5, 3),
};

static const uint32_t s_compact[] = {
    /* small panels (several PLCs): title row, main value + status, two buttons */
    HMI_TOKEN_PACK(PS_PRESS, WIDGET_CONTAINER_CARD, 0, 0, 0, 0),
    HMI_TOKEN_PACK(PS_PRESS, WIDGET_HERO_DIGIT,     0, 4, 9, 6),
    HMI_TOKEN_PACK(PS_P1_RUN, WIDGET_STATUS_PILL   , 10, 4, 6, 3),
    HMI_TOKEN_PACK(PS_P2_RUN, WIDGET_STATUS_PILL   , 10, 7, 6, 3),
    HMI_TOKEN_PACK(PS_MODE, WIDGET_ACTION_BUTTON,  0, 11, 8, 5),
    HMI_TOKEN_PACK(PS_SP, WIDGET_ACTION_BUTTON,  8, 11, 8, 5),
};

const hmi_device_profile_t g_profile_pump_station = {
    "pump_station", "Pump station", "siemens", "6es7 214|s7-1200", true, true,
    s_meta, N(s_meta),
    { 0, N(s_regs), s_regs },
    { 0, N(s_coils), s_coils },
    s_actions, N(s_actions),
    PS_COMM, HMI_REL_NONE, 0xFFFFu, PS_PRESS,
    s_full, N(s_full),
    s_compact, N(s_compact),
};
