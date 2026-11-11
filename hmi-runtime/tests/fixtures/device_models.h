/**
 * @file device_models.h
 * @brief Test fixture: Modbus device models (process and Modbus server logic of each device type)
 *
 * Portable C99, no OS headers, no heap. Each device type is
 *   - a register image (FX_HR internal registers, FX_COILS coils),
 *   - a row table saying what every value means and who may change it,
 *   - a register map from Modbus addresses to the image (real devices use sparse addresses:
 *     an Altivar's status word is at 3201, a PowerLogic meter's current at 3000 as a float),
 *   - an identity answered to FC43 (vendor, product, revision and, for PLC programs, the
 *     program name as UserApplicationName),
 *   - a process model stepped every 100 ms (one PLC scan of the device's program).
 *
 * The network side (sockets) lives in modbus_server.c; this file only turns request frames
 * into reply frames, so it can be tested without a network.
 */

#ifndef HMI_TESTS_DEVICE_MODELS_H
#define HMI_TESTS_DEVICE_MODELS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    FX_TANK, FX_ATV340, FX_ATV630, FX_CONVEYOR, FX_BOILER, FX_PUMPS, FX_COMPRESSOR, FX_AHU, FX_VFD,
    FX_METER, FX_ANON, FX_TYPE_COUNT
} fx_type_t;

#define FX_HR        16
#define FX_COILS     8
#define FX_MAX_ROWS  16
#define FX_MAX_REGS  16    /* entries of a sparse register map */
#define FX_EVENTS    8
#define FX_EVENT_LEN 80
#define FX_MAX_ADU   260

/* Row flags */
#define FX_COIL     0x01u   /* a coil (else a register of the image) */
#define FX_HMI_W    0x02u   /* the HMI may write it (FC05 / FC06) */
#define FX_PULSE    0x04u   /* coil the PLC clears again after acting on it (reset buttons) */
#define FX_LAB      0x08u   /* a test may change it (process input, fault injection) */
#define FX_DERIVED  0x10u   /* computed by the process: +/- shifts it by an offset */
#define FX_HEX      0x20u   /* shown in hex (status / control words) */

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
} fx_row_t;

/* Register map entry: Modbus address -> image register */
#define FX_FMT_INT16   0
#define FX_FMT_FLOAT32 1      /* two Modbus registers, high word first; value = hr / 10^decimals of its row */
typedef struct {
    uint16_t addr;
    uint8_t  idx;
    uint8_t  fmt;
} fx_reg_t;

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
    fx_row_t   rows[FX_MAX_ROWS];
    uint8_t     nregs;         /* 0 = registers 0..FX_HR-1 map straight to the image */
    fx_reg_t   regs[FX_MAX_REGS];
} fx_type_info_t;

typedef struct {
    fx_type_t type;
    int16_t    hr[FX_HR];
    uint8_t    coils[FX_COILS];
    int16_t    offset[FX_HR];      /* user nudges on computed values */
    int32_t    st[6];               /* process state: remainders, integrators, drive state */
    bool       stopped;             /* PLC in STOP: the program does not scan, the server still answers */
    bool       freeze;              /* fault: accept requests, never answer (link timeout) */
    bool       errors;              /* fault: answer every data request with exception 04 */
    uint32_t   requests, writes, exceptions, scans;
    char       events[FX_EVENTS][FX_EVENT_LEN];   /* what the HMI did, for the activity log */
    uint8_t    ev_head, ev_count;
} fx_device_t;

const fx_type_info_t *fx_type_info(fx_type_t t);
void    fx_init(fx_device_t *p, fx_type_t t);
void    fx_step(fx_device_t *p);                       /* 100 ms of process (nothing while stopped) */
void    fx_set_stopped(fx_device_t *p, bool stopped);  /* RUN / STOP switch */

/* Modbus address of an image register (first register for FLOAT32), -1 if it is not mapped */
int32_t fx_reg_address(const fx_type_info_t *ti, uint8_t idx);

int16_t fx_row_value(const fx_device_t *p, uint8_t row);
void    fx_row_text(const fx_device_t *p, uint8_t row, char *out, size_t cap);   /* "72.0 %", "Auto", "ON" */
void    fx_nudge(fx_device_t *p, uint8_t row, int dir);   /* +/-: dir = +1 / -1 */
void    fx_toggle_coil(fx_device_t *p, uint8_t row);      /* user forces a coil */

/* Complete frame length from the MBAP header: > 0 = bytes in the frame,
 * 0 = need more bytes, -1 = not Modbus TCP (drop the connection). */
int32_t fx_frame_length(const uint8_t *buf, size_t len);
/* Answer one request frame. Returns the reply length, 0 = no reply (frozen or not Modbus). */
size_t  fx_handle(fx_device_t *p, const uint8_t *adu, size_t len, uint8_t *out, size_t cap);

bool    fx_pop_event(fx_device_t *p, char *out, size_t cap);

#endif /* HMI_TESTS_DEVICE_MODELS_H */
