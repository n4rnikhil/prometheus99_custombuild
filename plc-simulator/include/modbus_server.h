/**
 * @file modbus_server.h
 * @brief The bench: up to PLC_MAX_STATIONS simulated devices, each a Modbus TCP server on its own port
 *
 * Portable (Winsock on Windows, BSD sockets on Linux / Raspberry Pi), single threaded, never blocks:
 * call bench_poll() every few milliseconds. It accepts connections, answers requests through the
 * device models (plc_models.h), runs one 100 ms scan of every device that is in RUN, and reports
 * what happened through the log callback.
 */

#ifndef PLC_SIMULATOR_MODBUS_SERVER_H
#define PLC_SIMULATOR_MODBUS_SERVER_H

#include "plc_models.h"

#define PLC_MAX_STATIONS   128
#define PLC_MAX_CLIENTS    4
#define PLC_DEFAULT_PORT   1502          /* first port; devices take 1502..1629 (the HMI looks there) */
#define PLC_PORT_REUSE_MS  65000u        /* an HMI ignores a non-PLC address for 60 s: do not reuse it sooner */

typedef struct {
    bool      used;
    sim_plc_t sim;
    char      name[40];                  /* "Altivar ATV340 1" */
    uint8_t   number;
    uint16_t  port;
    bool      plugged;                   /* listening */
    bool      hmi_checked;               /* a client connected at least once since plug-in */
    uint32_t  last_request_ms;
    int       clients;                   /* connected clients */
    char      client_ip[PLC_MAX_CLIENTS][16];
    uint8_t   ui_details;                /* window: 0 automatic, 1 shown, 2 hidden */
} plc_station_t;

typedef void (*bench_log_fn)(const char *text);

bool   bench_init(bench_log_fn log, uint16_t base_port);   /* false: sockets unavailable */
void   bench_shutdown(void);

/* Listen on all network interfaces (true) or on 127.0.0.1 only (false, the default). */
void   bench_set_network(bool on);
bool   bench_network(void);
const char *bench_lan_ip(void);                            /* "" if unknown */
uint16_t bench_base_port(void);

int    bench_add(sim_type_t type);                         /* station index, -1 = bench full */
int    bench_add_mixed(int n);                             /* n PLCs and drives of every type in turn; returns how many */
int    bench_count(void);                                  /* devices on the bench */
void   bench_remove(int idx);
bool   bench_plug(int idx);                                /* start listening */
void   bench_unplug(int idx);                              /* drop clients, stop listening */
plc_station_t *bench_station(int idx);                     /* NULL if idx is not in use */

void   bench_poll(uint32_t now_ms);

/* Device type by short name ("tank", "atv340", "atv630", "conveyor", "boiler", "pumps",
 * "compressor", "ahu", "abb", "meter", "unknown"), SIM_TYPE_COUNT if unknown */
sim_type_t bench_type_by_name(const char *name);
const char *bench_type_key(sim_type_t t);

/* "tank", "tank:20" (20 tanks) or "mixed:100" (100 PLCs and drives, every type in turn).
 * Returns how many were added; -1 if the text is not a device type. */
int bench_add_spec(const char *spec);

#endif /* PLC_SIMULATOR_MODBUS_SERVER_H */
