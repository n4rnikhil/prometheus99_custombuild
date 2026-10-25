/**
 * @file plc_sim.h
 * @brief Simulated Modbus devices: process models + Modbus server logic (PLC Lab and --sim)
 *
 * Portable C99, no OS headers, no heap. Each device type is a register image (16 holding
 * registers, 8 coils), a row table that says what every address means and who may write it,
 * and a process model stepped every 100 ms. The register maps match the HMI driver modules
 * in src/drivers/profiles/, so the HMI drives these devices exactly like the real thing.
 *
 * Users: lab/plc_lab_win32.c (each device is a Modbus TCP server) and the HMI's built-in
 * simulator transport (--sim), which talks to the same model through sim_handle().
 */

#ifndef INDUSTRIAL_HMI_PLC_SIM_H
#define INDUSTRIAL_HMI_PLC_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SIM_TANK, SIM_DRIVE, SIM_CONVEYOR, SIM_BOILER, SIM_PUMPS, SIM_COMPRESSOR, SIM_AHU, SIM_VFD,
    SIM_METER, SIM_ANON, SIM_TYPE_COUNT
} sim_type_t;

#define SIM_HR        16
#define SIM_COILS     8
#define SIM_MAX_ROWS  16
#define SIM_EVENTS    8
#define SIM_EVENT_LEN 80
#define SIM_MAX_ADU   260

/* Row flags */
#define SIM_COIL     0x01u   /* a coil (else a holding register) */
#define SIM_HMI_W    0x02u   /* the HMI may write it (FC05 / FC06) */
#define SIM_PULSE    0x04u   /* coil the PLC clears again after acting on it (reset buttons) */
#define SIM_LAB      0x08u   /* PLC Lab may change it (process input, fault injection) */
#define SIM_DERIVED  0x10u   /* computed by the process: lab +/- shifts it by an offset */

typedef struct {
    const char *label;
    const char *unit;
    uint8_t     flags;
    uint8_t     addr;
    uint8_t     decimals;
    int16_t     step;          /* lab +/- step; 0 = no buttons */
    int16_t     min, max;      /* clamp for writes and nudges */
    uint8_t     block_mask;    /* coils: switching ON is refused while any of these coils is set */
    const char *options;       /* enum registers: "Manual|Auto" (value = option index) */
} sim_row_t;

typedef struct {
    const char *type_name;     /* "Tank skid" */
    const char *model;         /* "Modicon M340" */
    const char *vendor;        /* FC43 identity; NULL = device does not answer FC43 */
    const char *product;
    const char *revision;
    bool        is_plc;        /* the HMI should attach it (true) or ignore it (false) */
    uint8_t     nrows;
    sim_row_t   rows[SIM_MAX_ROWS];
} sim_type_info_t;

typedef struct {
    sim_type_t type;
    int16_t    hr[SIM_HR];
    uint8_t    coils[SIM_COILS];
    int16_t    offset[SIM_HR];      /* lab nudges on computed values */
    int32_t    st[6];               /* process state: remainders, integrators */
    bool       freeze;              /* fault: accept requests, never answer (link timeout) */
    bool       errors;              /* fault: answer every data request with exception 04 */
    uint32_t   requests, writes, exceptions;
    char       events[SIM_EVENTS][SIM_EVENT_LEN];   /* what the HMI did, for the activity log */
    uint8_t    ev_head, ev_count;
} sim_plc_t;

const sim_type_info_t *sim_type_info(sim_type_t t);
void    sim_init(sim_plc_t *p, sim_type_t t);
void    sim_step(sim_plc_t *p);                       /* 100 ms of process */

int16_t sim_row_value(const sim_plc_t *p, uint8_t row);
void    sim_row_text(const sim_plc_t *p, uint8_t row, char *out, size_t cap);   /* "72.0 %", "Auto", "ON" */
void    sim_nudge(sim_plc_t *p, uint8_t row, int dir);   /* lab +/-: dir = +1 / -1 */
void    sim_toggle_coil(sim_plc_t *p, uint8_t row);      /* lab forces a coil */

/* Complete frame length from the MBAP header: > 0 = bytes in the frame,
 * 0 = need more bytes, -1 = not Modbus TCP (drop the connection). */
int32_t sim_frame_length(const uint8_t *buf, size_t len);
/* Answer one request frame. Returns the reply length, 0 = no reply (frozen or not Modbus). */
size_t  sim_handle(sim_plc_t *p, const uint8_t *adu, size_t len, uint8_t *out, size_t cap);

bool    sim_pop_event(sim_plc_t *p, char *out, size_t cap);

#endif /* INDUSTRIAL_HMI_PLC_SIM_H */
