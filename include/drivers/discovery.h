/**
 * @file discovery.h
 * @brief Automatic detection of Modbus TCP devices (runs in its own low-priority thread)
 *
 * Every HMI_DISCOVERY_PERIOD_MS the thread probes each configured target that is
 * not already on the dashboard:
 *   1. non-blocking TCP connect (batches of up to 32, HMI_DISCOVERY_CONNECT_MS)
 *   2. Modbus FC43 "Read Device Identification" -> vendor / product -> profile
 *   3. no FC43 support: one FC03 read of HR0 proves it speaks Modbus (generic profile)
 * Only PLCs are reported: a scanned device must identify itself (FC43) as a model
 * that has a driver module with auto_attach (e.g. Modicon BMX/BME, Altivar). Other
 * Modbus devices on the network are ignored and not re-probed for
 * HMI_DISCOVERY_IGNORE_MS. Addresses given with --plc are explicit: they are attached
 * even when they cannot identify themselves (generic driver).
 * Found devices are reported through a callback; the fieldbus thread attaches them.
 *
 * Targets (hmi_discovery_add):
 *   "192.168.1.50"            port 502
 *   "192.168.1.50:1502"       one port
 *   "127.0.0.1:1502-1509"     port range
 *   "192.168.1.0/24"          every host of a subnet, port 502 (opt-in network scan)
 *   "10.0.0.0/24:502"         subnet + port;  "/u3" suffix on any form = unit id 3
 */

#ifndef INDUSTRIAL_HMI_DISCOVERY_H
#define INDUSTRIAL_HMI_DISCOVERY_H

#include "device_info.h"
#include "../hal/atomic.h"

#define HMI_DISCOVERY_MAX_RANGES  16
#define HMI_DISCOVERY_MAX_TARGETS 1024   /* per sweep, after expansion */
#define HMI_DISCOVERY_IGNORE_MS   60000  /* a non-PLC device is not probed again for this long */
#define HMI_DISCOVERY_IGNORE_LIST 32

typedef bool (*hmi_discovery_attached_fn)(const char *host, uint16_t port, uint8_t unit);
typedef void (*hmi_discovery_found_fn)(const hmi_device_candidate_t *c);

/* Configuration (call before the thread starts). false = malformed spec, or the targets of all
 * specs together would exceed HMI_DISCOVERY_MAX_TARGETS (refused rather than silently truncated). */
bool hmi_discovery_add(const char *spec, bool explicit_target);
void hmi_discovery_clear(void);                 /* forget every configured target (not while the thread runs) */
void hmi_discovery_add_defaults(void);          /* localhost: 502 and 1502-1509 */
uint32_t hmi_discovery_target_count(void);

/* Thread body: sweeps until *running becomes 0. */
void hmi_discovery_run(hal_atomic_t *running, hmi_discovery_attached_fn attached, hmi_discovery_found_fn found);

/* Statistics for the diagnostics screen */
typedef struct {
    uint32_t sweeps;
    uint32_t probes;
    uint32_t found;
    uint32_t ignored;            /* Modbus devices that are not a known PLC */
    uint32_t last_sweep_ms;      /* duration of the last sweep */
    uint32_t targets;
} hmi_discovery_stats_t;
void hmi_discovery_get_stats(hmi_discovery_stats_t *out);

#endif /* INDUSTRIAL_HMI_DISCOVERY_H */
