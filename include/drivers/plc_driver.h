/**
 * @file plc_driver.h
 * @brief Unified Plug-and-Play Industrial PLC Driver & Manifest Interface
 * @standards Modbus-TCP (Port 502) / Modbus-RTU / Self-Describing UI
 */

#ifndef INDUSTRIAL_HMI_PLC_DRIVER_H
#define INDUSTRIAL_HMI_PLC_DRIVER_H

#include "../config.h"
#include "../core/tags.h"

typedef struct {
    const char *driver_name;      /* e.g. "Schneider Modicon M340/M580" */
    uint16_t    default_port;     /* 502 for Modbus-TCP */

    /* Driver Lifecycle */
    bool (*init)(void);
    bool (*connect)(const char *endpoint);
    void (*disconnect)(void);

    /* Cyclic Polling (1000 Hz / 1 ms tick) */
    bool (*poll_cyclic)(hmi_tag_table_t *tags, uint32_t now_ms);

    /* Operator Control Commands */
    bool (*write_coil)(uint16_t coil_addr, bool state);
    bool (*write_register)(uint16_t reg_addr, int16_t value);

    /* Plug-and-Play UI Manifest: The driver brings its own screens! */
    const uint32_t *ui_bytecode;
    uint8_t         widget_count;
    const hmi_tag_meta_t *tag_metadata;
    uint8_t               tag_count;
} hmi_plc_driver_t;

#endif /* INDUSTRIAL_HMI_PLC_DRIVER_H */
