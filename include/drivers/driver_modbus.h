/**
 * @file driver_modbus.h
 * @brief Schneider Electric Modicon M340/M580 PLC Driver & Process Simulator
 * @standards Modbus-TCP / Modbus-RTU / IEC 61131-3 Tag Definitions
 */

#ifndef INDUSTRIAL_HMI_DRIVER_MODBUS_H
#define INDUSTRIAL_HMI_DRIVER_MODBUS_H

#include "plc_driver.h"

/* Standard Schneider Tag Indices */
#define SCHNEIDER_TAG_TANK_LEVEL    0
#define SCHNEIDER_TAG_PUMP_RPM      1
#define SCHNEIDER_TAG_VALVE_POS     2
#define SCHNEIDER_TAG_REACTOR_PRESS 3
#define SCHNEIDER_TAG_COOLANT_TEMP  4
#define SCHNEIDER_TAG_PUMP_RUNNING  5
#define SCHNEIDER_TAG_PUMP_B_RUN    6
#define SCHNEIDER_TAG_ESTOP_ACTIVE  7
#define SCHNEIDER_TAG_COUNT         8

const hmi_plc_driver_t* hmi_driver_modbus_get(void);

#endif /* INDUSTRIAL_HMI_DRIVER_MODBUS_H */
