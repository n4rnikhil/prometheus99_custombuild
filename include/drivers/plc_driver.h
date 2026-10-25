/**
 * @file plc_driver.h
 * @brief Plug-and-play driver interface + self-describing UI manifest
 *
 * Threading contract: after main() has called init() (before any worker thread
 * exists), EVERY other function pointer is called only from the fieldbus thread.
 * Drivers therefore need no locking. Nothing here may block: poll_cyclic() is
 * called on every 1 ms fieldbus iteration and must return within microseconds.
 */

#ifndef INDUSTRIAL_HMI_PLC_DRIVER_H
#define INDUSTRIAL_HMI_PLC_DRIVER_H

#include "../config.h"
#include "../core/tags.h"

typedef enum {
    HMI_DRV_MODE_SIMULATOR  = 0,
    HMI_DRV_MODE_MODBUS_TCP = 1,
    HMI_DRV_MODE_HOST       = 2
} hmi_driver_mode_t;

typedef enum {
    HMI_LINK_DOWN       = 0,  /* not connected (backing off before retry) */
    HMI_LINK_CONNECTING = 1,  /* TCP connect in progress */
    HMI_LINK_UP         = 2   /* connected and answering */
} hmi_link_state_t;

/* Communication statistics (published to the UI for the diagnostics screen) */
typedef struct {
    uint8_t  mode;               /* hmi_driver_mode_t */
    uint8_t  link_state;         /* hmi_link_state_t */
    uint8_t  unit_id;
    uint8_t  reserved;
    char     endpoint[48];       /* "127.0.0.1:502" or "SIMULATOR" */
    uint32_t requests_sent;
    uint32_t responses_ok;
    uint32_t timeouts;
    uint32_t exceptions;         /* Modbus exception responses */
    uint32_t protocol_errors;    /* malformed / mismatched frames */
    uint32_t reconnects;         /* successful (re)connections */
    uint32_t writes_ok;
    uint32_t writes_failed;      /* rejected, timed out, queue full, or exception */
    uint32_t last_rtt_us;
    uint32_t max_rtt_us;
    uint32_t poll_period_ms;
    uint32_t last_good_ms;       /* uptime ms of last good response (0 = never) */
} hmi_driver_stats_t;

typedef struct {
    const char *driver_name;
    uint16_t    default_port;

    bool (*init)(void);                                  /* main thread, before workers start */
    bool (*connect)(const char *endpoint);               /* fieldbus thread; non-blocking start */
    void (*disconnect)(void);                            /* fieldbus thread */

    bool (*poll_cyclic)(hmi_tag_table_t *tags, uint32_t now_ms); /* every iteration; false = link not healthy */

    /* Queue a write; true = accepted (not necessarily completed yet). Addresses are
     * 0-based Modbus PDU addresses. */
    bool (*write_coil)(uint16_t coil_addr, bool state);
    bool (*write_register)(uint16_t reg_addr, int16_t value);

    void (*get_stats)(hmi_driver_stats_t *out);          /* may be NULL */

    const uint32_t       *ui_bytecode;
    uint8_t               widget_count;
    const hmi_tag_meta_t *tag_metadata;
    uint8_t               tag_count;
} hmi_plc_driver_t;

#endif /* INDUSTRIAL_HMI_PLC_DRIVER_H */
