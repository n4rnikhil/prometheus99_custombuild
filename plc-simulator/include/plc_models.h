/**
 * @file plc_models.h
 * @brief Simulated Modbus devices: process models and the Modbus server logic of each device
 *
 * Portable C99, no OS headers, no heap. Each device type is
 *   - a register image (SIM_HR internal registers, SIM_COILS coils),
 *   - a row table saying what every value means and who may change it,
 *   - a register map from Modbus addresses to the image (real devices use sparse addresses:
 *     an Altivar's status word is at 3201, a PowerLogic meter's current at 3000 as a float),
 *   - an identity answered to FC43 (vendor, product, revision and, for PLC programs, the
 *     program name as UserApplicationName),
 *   - a process model stepped every 100 ms (one PLC scan of the simulated program).
 *
 * The network side (sockets) lives in modbus_server.c; this file only turns request frames
 * into reply frames, so it can be tested without a network.
 */

#ifndef PLC_SIMULATOR_PLC_MODELS_H
#define PLC_SIMULATOR_PLC_MODELS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SIM_TANK, SIM_ATV340, SIM_ATV630, SIM_CONVEYOR, SIM_BOILER, SIM_PUMPS, SIM_COMPRESSOR, SIM_AHU, SIM_VFD,
    SIM_METER, SIM_ANON, SIM_TYPE_COUNT
} sim_type_t;

#define SIM_HR        16
#define SIM_COILS     8
#define SIM_MAX_ROWS  16
#define SIM_MAX_REGS  16    /* entries of a sparse register map */
#define SIM_EVENTS    8
#define SIM_EVENT_LEN 80
#define SIM_MAX_ADU   260

/* Row flags */
#define SIM_COIL     0x01u   /* a coil (else a register of the image) */
#define SIM_HMI_W    0x02u   /* the HMI may write it (FC05 / FC06) */
#define SIM_PULSE    0x04u   /* coil the PLC clears again after acting on it (reset buttons) */
#define SIM_LAB      0x08u   /* the simulator's user may change it (process input, fault injection) */
#define SIM_DERIVED  0x10u   /* computed by the process: +/- shifts it by an offset */
#define SIM_HEX      0x20u   /* shown in hex (status / control words) */

typedef struct {
    const char *label;
    const char *unit;
    uint8_t     flags;
    uint8_t     addr;          /* index into hr[] or coils[] */
    uint8_t     decimals;
    int16_t     step;          /* +/- step; 0 = no buttons */
    int16_t     min, max;      /* clamp for writes and nudges */
    uint8_t     block_mask;    /* coils: switching ON is refused while any of these coils is set */
    const char *options;       /* enum registers: "Manual|Auto" (value = option index) */
} sim_row_t;

/* Register map entry: Modbus address -> image register */
#define SIM_FMT_INT16   0
#define SIM_FMT_FLOAT32 1      /* two Modbus registers, high word first; value = hr / 10^decimals of its row */
typedef struct {
    uint16_t addr;
    uint8_t  idx;
    uint8_t  fmt;
} sim_reg_t;

typedef struct {
    const char *type_name;     /* "Tank skid" */
    const char *model;         /* "Modicon M340" */
    const char *vendor;        /* FC43 identity; NULL = device does not answer FC43 */
    const char *product;
    const char *revision;
    const char *app;           /* FC43 UserApplicationName of the PLC program; NULL = a product, not a program */
    bool        is_plc;        /* an HMI should attach it on its own (PLCs and drives); meters: false */
    bool        has_coils;     /* answers FC01 / FC05 (drives and meters do not) */
    uint8_t     nrows;
    sim_row_t   rows[SIM_MAX_ROWS];
    uint8_t     nregs;         /* 0 = registers 0..SIM_HR-1 map straight to the image */
    sim_reg_t   regs[SIM_MAX_REGS];
} sim_type_info_t;

typedef struct {
    sim_type_t type;
    int16_t    hr[SIM_HR];
    uint8_t    coils[SIM_COILS];
    int16_t    offset[SIM_HR];      /* user nudges on computed values */
    int32_t    st[6];               /* process state: remainders, integrators, drive state */
    bool       stopped;             /* PLC in STOP: the program does not scan, the server still answers */
    bool       freeze;              /* fault: accept requests, never answer (link timeout) */
    bool       errors;              /* fault: answer every data request with exception 04 */
    uint32_t   requests, writes, exceptions, scans;
    char       events[SIM_EVENTS][SIM_EVENT_LEN];   /* what the HMI did, for the activity log */
    uint8_t    ev_head, ev_count;
} sim_plc_t;

const sim_type_info_t *sim_type_info(sim_type_t t);
void    sim_init(sim_plc_t *p, sim_type_t t);
void    sim_step(sim_plc_t *p);                       /* 100 ms of process (nothing while stopped) */
void    sim_set_stopped(sim_plc_t *p, bool stopped);  /* RUN / STOP switch */

/* Modbus address of an image register (first register for FLOAT32), -1 if it is not mapped */
int32_t sim_reg_address(const sim_type_info_t *ti, uint8_t idx);

int16_t sim_row_value(const sim_plc_t *p, uint8_t row);
void    sim_row_text(const sim_plc_t *p, uint8_t row, char *out, size_t cap);   /* "72.0 %", "Auto", "ON" */
void    sim_nudge(sim_plc_t *p, uint8_t row, int dir);   /* +/-: dir = +1 / -1 */
void    sim_toggle_coil(sim_plc_t *p, uint8_t row);      /* user forces a coil */

/* Complete frame length from the MBAP header: > 0 = bytes in the frame,
 * 0 = need more bytes, -1 = not Modbus TCP (drop the connection). */
int32_t sim_frame_length(const uint8_t *buf, size_t len);
/* Answer one request frame. Returns the reply length, 0 = no reply (frozen or not Modbus). */
size_t  sim_handle(sim_plc_t *p, const uint8_t *adu, size_t len, uint8_t *out, size_t cap);

bool    sim_pop_event(sim_plc_t *p, char *out, size_t cap);

#endif /* PLC_SIMULATOR_PLC_MODELS_H */
