/**
 * @file driver_modbus.h
 * @brief Schneider Modicon M340/M580 driver: Modbus TCP client or built-in process simulator
 * @standards Modbus Application Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b
 *
 * Mode is chosen by connect():
 *   connect(NULL) or connect("")      -> built-in process simulator (default)
 *   connect("192.168.1.50")           -> Modbus TCP, port 502, unit id 1
 *   connect("127.0.0.1:1502")         -> Modbus TCP on port 1502
 *   connect("127.0.0.1:1502/7")       -> ... unit id 7
 *   "localhost" is accepted as 127.0.0.1. Host names are NOT resolved (DNS
 *   would block the 1 kHz loop): use a numeric IPv4 address.
 *
 * PLC data map (0-based PDU addresses; on an M340/M580 holding register n is
 * %MWn and coil n is %Mn):
 */

#ifndef INDUSTRIAL_HMI_DRIVER_MODBUS_H
#define INDUSTRIAL_HMI_DRIVER_MODBUS_H

#include "plc_driver.h"

/* --- Tag ids --- */
#define SCHNEIDER_TAG_TANK_LEVEL    0   /* 0.1 %   raw 0..1000 */
#define SCHNEIDER_TAG_PUMP_RPM      1   /* RPM     raw 0..3600 */
#define SCHNEIDER_TAG_VALVE_POS     2   /* %       raw 0..100  */
#define SCHNEIDER_TAG_REACTOR_PRESS 3   /* kPa     raw 0..250  */
#define SCHNEIDER_TAG_COOLANT_TEMP  4   /* C       raw 0..120  */
#define SCHNEIDER_TAG_PUMP_RUNNING  5   /* digital */
#define SCHNEIDER_TAG_PUMP_B_RUN    6   /* digital */
#define SCHNEIDER_TAG_ESTOP_ACTIVE  7   /* digital, alarms at ALARM_SEV_ESTOP */
#define SCHNEIDER_TAG_COMM_LOSS     12  /* digital, set by the driver while the PLC link is down (CRITICAL) */
#define SCHNEIDER_TAG_COUNT         9   /* entries in the driver's metadata table */

/* --- Holding registers (FC03 read 5 from address 0; FC06 write) --- */
#define MB_HR_TANK_LEVEL     0   /* %MW0 */
#define MB_HR_PUMP_RPM       1   /* %MW1 */
#define MB_HR_VALVE_POS      2   /* %MW2  (writable: valve position command) */
#define MB_HR_PRESSURE       3   /* %MW3 */
#define MB_HR_TEMP           4   /* %MW4 */
#define MB_HR_COUNT          5

/* --- Coils (FC01 read 3 from address 0; FC05 write) --- */
#define MB_COIL_PUMP_A       0   /* %M0 run command/status, writable */
#define MB_COIL_PUMP_B       1   /* %M1 coolant pump B */
#define MB_COIL_ESTOP        2   /* %M2 E-stop latch (write 1 = trip, 0 = reset) */
#define MB_COIL_COUNT        3

/* Valve toggle used by the HMI: above 50 % -> 25 %, else -> 85 %. */
static inline int16_t hmi_valve_toggle_target(int16_t current_pos)
{
    return (int16_t)((current_pos > 50) ? 25 : 85);
}

/* Device profile (schneider_profile.c): portable, data only */
extern const hmi_tag_meta_t g_schneider_meta[SCHNEIDER_TAG_COUNT];
#define SCHNEIDER_MANIFEST_COUNT 14   /* the compiler rejects a manifest of any other length */
extern const uint32_t       g_schneider_manifest[SCHNEIDER_MANIFEST_COUNT];

const hmi_plc_driver_t* hmi_driver_modbus_get(void);

#endif /* INDUSTRIAL_HMI_DRIVER_MODBUS_H */
