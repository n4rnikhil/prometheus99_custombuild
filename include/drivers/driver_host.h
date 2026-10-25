/**
 * @file driver_host.h
 * @brief Host telemetry driver (CPU load, RAM load, HMI process memory)
 */

#ifndef INDUSTRIAL_HMI_DRIVER_HOST_H
#define INDUSTRIAL_HMI_DRIVER_HOST_H

#include "plc_driver.h"

#define HOST_TAG_CPU_LOAD     8   /* % system CPU load */
#define HOST_TAG_RAM_LOAD     9   /* % system physical memory in use */
#define HOST_TAG_PROC_PRIV_KB 10  /* KB private committed memory of this HMI process */
#define HOST_TAG_PROC_WS_KB   11  /* KB working set of this HMI process */
#define HOST_TAG_COUNT        4

const hmi_plc_driver_t* hmi_driver_host_get(void);

#endif /* INDUSTRIAL_HMI_DRIVER_HOST_H */
