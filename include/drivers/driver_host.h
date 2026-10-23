/**
 * @file driver_host.h
 * @brief Host Hardware Telemetry Driver & Manifest
 */

#ifndef INDUSTRIAL_HMI_DRIVER_HOST_H
#define INDUSTRIAL_HMI_DRIVER_HOST_H

#include "plc_driver.h"

#define HOST_TAG_CPU_LOAD    8
#define HOST_TAG_RAM_LOAD    9
#define HOST_TAG_CPU_TEMP    10
#define HOST_TAG_DISK_IO     11
#define HOST_TAG_COUNT       4

const hmi_plc_driver_t* hmi_driver_host_get(void);

#endif /* INDUSTRIAL_HMI_DRIVER_HOST_H */
