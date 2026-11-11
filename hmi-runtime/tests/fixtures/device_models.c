/**
 * @file device_models.c
 * @brief Test fixture: Modbus device models and their Modbus server logic (FC01/03/05/06/43)
 */

#include "device_models.h"
#include <stdio.h>
#include <string.h>

/* Row constructors: register, enum register, coil */
#define R(l, u, f, a, d, s, mn, mx)  { l, u, (uint8_t)(f), a, d, s, mn, mx, 0, NULL }
#define E(l, f, a, mx, o)            { l, "", (uint8_t)(f), a, 0, 1, 0, mx, 0, o }
#define C(l, f, a, blk)              { l, "", (uint8_t)((f) | FX_COIL), a, 0, 0, 0, 1, (uint8_t)(blk), NULL }
#define BIT(n)                       (1u << (n))
#define HW  FX_HMI_W
#define LB  FX_LAB
#define DV  FX_DERIVED
#define PL  FX_PULSE
#define HX  FX_HEX
#define NO_REGS 0, { { 0, 0, 0 } }     /* registers 0..15 map straight to the image */
#define F32 FX_FMT_FLOAT32

/* Altivar image (both drive types): the drive's own communication parameters */
enum { AV_ETA, AV_RFR, AV_FRH, AV_LCR, AV_CMD, AV_LFR, AV_ACC, AV_DEC, AV_LFT };
enum { AV_C_OVERLOAD, AV_C_LOCAL };
#define ALTIVAR_ROWS 11, { \
        R("Status word ETA",     "",   DV | HX, AV_ETA, 0, 0, -32768, 32767), \
        R("Output frequency",    "Hz", DV,      AV_RFR, 1, 0, -600, 600), \
        R("Frequency ref FRH",   "Hz", DV,      AV_FRH, 1, 0, -600, 600), \
        R("Motor current",       "A",  DV | LB, AV_LCR, 1, 2, -100, 100), \
        R("Control word CMD",    "",   HW | HX, AV_CMD, 0, 0, -32768, 32767), \
        R("Speed reference LFR", "Hz", HW | LB, AV_LFR, 1, 50, 0, 600), \
        R("Acceleration ACC",    "s",  HW | LB, AV_ACC, 1, 10, 1, 9999), \
        R("Deceleration DEC",    "s",  HW | LB, AV_DEC, 1, 10, 1, 9999), \
        R("Last fault LFT",      "",   DV,      AV_LFT, 0, 0, 0, 999), \
        C("Overload (fault cause)", LB, AV_C_OVERLOAD, 0), \
        C("Local control (keypad)", LB, AV_C_LOCAL, 0) }, \
    9, { { 3201, AV_ETA, 0 }, { 3202, AV_RFR, 0 }, { 3203, AV_FRH, 0 }, { 3204, AV_LCR, 0 }, \
         { 8501, AV_CMD, 0 }, { 8502, AV_LFR, 0 }, { 9001, AV_ACC, 0 }, { 9002, AV_DEC, 0 }, { 7121, AV_LFT, 0 } }

/* CiA402 drive states and their status word (ETA) values */
enum { DS_SWITCH_ON_DISABLED, DS_READY, DS_SWITCHED_ON, DS_RUNNING, DS_FAULT };
#define AV_FAULT_CODE 16      /* "overload" fault code shown in LFT */

static const fx_type_info_t s_types[FX_TYPE_COUNT] = {
    [FX_TANK] = { "Tank skid", "Modicon M340", "Schneider Electric", "BMX P34 2020", "V3.20", "HMI-TANK", true, true, 13, {
        R("Tank level",     "%",   LB,      0, 1, 50, 0, 1000),
        R("Pump A speed",   "rpm", DV,      1, 0, 0,  0, 3000),
        R("Valve V-101",    "%",   HW | LB, 2, 0, 5,  0, 100),
        R("Line pressure",  "kPa", DV | LB, 3, 0, 10, -200, 400),
        R("Coolant temp",   "C",   DV | LB, 4, 0, 5,  -50, 150),
        R("Level setpoint", "%",   HW | LB, 5, 1, 50, 100, 950),
        E("Control mode",          HW | LB, 6, 1, "Manual|Auto"),
        R("HMI heartbeat",  "",    HW | LB, 7, 0, 0,  0, 32767),
        C("Feed pump A",           HW | LB, 0, BIT(2) | BIT(4)),
        C("Coolant pump B",        HW | LB, 1, BIT(2)),
        C("Stop latch",            HW | LB, 2, 0),
        C("Fault reset",           HW | LB | PL, 3, 0),
        C("Pump A overload",       LB, 4, 0) }, NO_REGS },
    [FX_ATV340] = { "Altivar ATV340", "Altivar Machine ATV340", "Schneider Electric", "ATV340U07N4E", "V3.3",
                     NULL, true, false, ALTIVAR_ROWS },
    [FX_ATV630] = { "Altivar ATV630", "Altivar Process ATV630", "Schneider Electric", "ATV630U07N4", "V3.4",
                     NULL, true, false, ALTIVAR_ROWS },
    [FX_CONVEYOR] = { "Conveyor", "Modicon M241", "Schneider Electric", "TM241CEC24T", "V5.1", "HMI-CONVEYOR", true, true, 10, {
        R("Belt speed",     "m/min", DV,      0, 1, 0,  0, 1500),
        R("Speed setpoint", "m/min", HW | LB, 1, 1, 50, 0, 1200),
        R("Item count",     "",      DV,      2, 0, 0,  0, 32000),
        R("Motor current",  "A",     DV | LB, 3, 1, 10, -100, 300),
        R("Throughput",     "/min",  DV,      4, 0, 0,  0, 2000),
        C("Run",                     HW | LB, 0, BIT(1) | BIT(2)),
        C("Jam detected",            LB, 1, 0),
        C("Pull-cord stop",          HW | LB, 2, 0),
        C("Jam reset",               HW | LB | PL, 3, 0),
        C("Counter reset",           HW | LB | PL, 4, 0) }, NO_REGS },
    [FX_BOILER] = { "Boiler", "Modicon M580", "Schneider Electric", "BME P58 2040", "V4.10", "HMI-BOILER", true, true, 12, {
        R("Water temp",      "C",   LB,      0, 1, 20, 0, 1200),
        R("Temp setpoint",   "C",   HW | LB, 1, 1, 50, 200, 950),
        R("Firing rate",     "%",   DV,      2, 0, 0,  0, 100),
        R("Boiler pressure", "kPa", DV | LB, 3, 0, 10, -100, 600),
        R("Drum level",      "%",   LB,      4, 0, 5,  0, 100),
        E("Control mode",           HW | LB, 5, 1, "Manual|Auto"),
        R("Manual firing",   "%",   HW | LB, 6, 0, 10, 0, 100),
        C("Burner",                 HW | LB, 0, BIT(2) | BIT(3)),
        C("Flame",                  0, 1, 0),
        C("Low water trip",         LB, 2, 0),
        C("Stop latch",             HW | LB, 3, 0),
        C("Trip reset",             HW | LB | PL, 4, 0) }, NO_REGS },
    [FX_PUMPS] = { "Pump station", "Siemens S7-1200", "Siemens", "6ES7 214-1AG40-0XB0", "V4.6", "HMI-PUMPS", true, true, 14, {
        R("Discharge pressure", "bar",  DV | LB, 0, 2, 20, 0, 1200),
        R("Pressure setpoint",  "bar",  HW | LB, 1, 2, 25, 100, 800),
        R("Flow",               "m3/h", DV,      2, 1, 0,  0, 2000),
        R("Pump 1 speed",       "%",    DV,      3, 0, 0,  0, 100),
        R("Pump 2 speed",       "%",    DV,      4, 0, 0,  0, 100),
        E("Mode",                       HW | LB, 5, 2, "Off|Hand|Auto"),
        R("Hand speed",         "%",    HW | LB, 6, 0, 10, 0, 100),
        E("Duty pump",                  HW | LB, 7, 1, "Pump 1|Pump 2"),
        R("Water demand",       "%",    LB,      8, 0, 10, 0, 100),
        C("Pump 1 running",             0, 0, 0),
        C("Pump 2 running",             0, 1, 0),
        C("Pump 1 fault",               LB, 2, 0),
        C("Pump 2 fault",               LB, 3, 0),
        C("Fault reset",                HW | LB | PL, 4, 0) }, NO_REGS },
    [FX_COMPRESSOR] = { "Air compressor", "Allen-Bradley Micro850", "Rockwell Automation", "2080-LC50-24QWB", "21.011",
                         "HMI-COMPRESSOR", true, true, 10, {
        R("Air pressure",    "bar", LB,      0, 2, 20, 0, 1200),
        R("Load pressure",   "bar", HW | LB, 1, 2, 25, 400, 900),
        R("Unload pressure", "bar", HW | LB, 2, 2, 25, 450, 1000),
        R("Discharge temp",  "C",   DV | LB, 3, 1, 50, -200, 1500),
        R("Motor current",   "A",   DV,      4, 1, 0,  0, 1000),
        E("State",                  DV,      5, 2, "Stopped|Loaded|Unloaded"),
        R("Air demand",      "%",   LB,      6, 0, 10, 0, 100),
        C("Run",                    HW | LB, 0, BIT(1)),
        C("High temp trip",         LB, 1, 0),
        C("Trip reset",             HW | LB | PL, 2, 0) }, NO_REGS },
    [FX_AHU] = { "Air handling unit", "Modicon M251", "Schneider Electric", "TM251MESE", "V5.0", "HMI-AHU", true, true, 12, {
        R("Supply air temp",    "C",   LB,      0, 1, 10, -100, 500),
        R("Supply setpoint",    "C",   HW | LB, 1, 1, 10, 120, 300),
        R("Return air temp",    "C",   LB,      2, 1, 10, -100, 500),
        R("Fan speed",          "%",   DV,      3, 0, 0,  0, 100),
        R("Fan speed setpoint", "%",   HW | LB, 4, 0, 10, 20, 100),
        E("Mode",                      HW | LB, 5, 3, "Off|Heat|Cool|Auto"),
        R("Damper",             "%",   DV,      6, 0, 0,  0, 100),
        R("Filter pressure",    "Pa",  LB,      7, 0, 20, 0, 500),
        R("CO2",                "ppm", LB,      8, 0, 100, 300, 3000),
        C("Fan",                       HW | LB, 0, BIT(2)),
        C("Filter dirty",              0, 1, 0),
        C("Fire alarm",                LB, 2, 0) }, NO_REGS },
    [FX_VFD] = { "ABB drive", "ABB ACS580 (demo map)", "ABB", "ACS580-01-12A7-4", "2.15", "HMI-ABB", true, true, 9, {
        R("Motor speed",     "rpm", DV,      0, 0, 0,   0, 3000),
        R("Speed reference", "rpm", HW | LB, 1, 0, 100, 0, 1500),
        R("Motor current",   "A",   DV | LB, 2, 1, 10,  -200, 500),
        R("Torque",          "%",   DV,      3, 0, 0,   0, 300),
        R("DC bus",          "V",   DV,      4, 0, 0,   0, 900),
        R("Fault code",      "",    DV,      5, 0, 0,   0, 32000),
        C("Run",                    HW | LB, 0, BIT(1)),
        C("Fault",                  LB, 1, 0),
        C("Fault reset",            HW | LB | PL, 2, 0) }, NO_REGS },
    [FX_METER] = { "Energy meter", "PowerLogic EM6400NG", "Schneider Electric", "EM6400NG", "V1.2.0", NULL, false, false, 10, {
        R("Current A",       "A",   LB, 0, 2, 50, 0, 10000),
        R("Current B",       "A",   LB, 1, 2, 50, 0, 10000),
        R("Current C",       "A",   LB, 2, 2, 50, 0, 10000),
        R("Voltage A-B",     "V",   LB, 3, 1, 50, 0, 5000),
        R("Voltage B-C",     "V",   LB, 4, 1, 50, 0, 5000),
        R("Voltage C-A",     "V",   LB, 5, 1, 50, 0, 5000),
        R("Voltage L-L avg", "V",   DV, 6, 1, 0,  0, 5000),
        R("Active power",    "kW",  DV, 7, 2, 0,  0, 32000),
        R("Frequency",       "Hz",  LB, 8, 2, 10, 4500, 5500),
        R("Energy",          "kWh", DV, 9, 0, 0,  0, 32000) },
        10, { { 2999, 0, F32 }, { 3001, 1, F32 }, { 3003, 2, F32 }, { 3019, 3, F32 }, { 3021, 4, F32 },
              { 3023, 5, F32 }, { 3025, 6, F32 }, { 3059, 7, F32 }, { 3109, 8, F32 }, { 2699, 9, F32 } } },
    [FX_ANON] = { "Modbus device", "Unknown make", NULL, NULL, NULL, NULL, false, true, 4, {
        R("Register 0", "", LB, 0, 0, 10, -1000, 1000),
        R("Register 1", "", LB, 1, 0, 10, -1000, 1000),
        R("Register 2", "", LB, 2, 0, 10, -1000, 1000),
        R("Register 3", "", LB, 3, 0, 10, -1000, 1000) }, NO_REGS },
};

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

const fx_type_info_t *fx_type_info(fx_type_t t)
{
    return (unsigned)t < FX_TYPE_COUNT ? &s_types[t] : NULL;
}

static bool is_altivar(fx_type_t t) { return t == FX_ATV340 || t == FX_ATV630; }

/* ------------------------------------------------------------------------- */
/* Events (what the HMI did) for the activity log                             */
/* ------------------------------------------------------------------------- */
static void event(fx_device_t *p, const char *text)
{
    uint8_t slot = (uint8_t)((p->ev_head + p->ev_count) % FX_EVENTS);
    if (p->ev_count == FX_EVENTS) { p->ev_head = (uint8_t)((p->ev_head + 1) % FX_EVENTS); p->ev_count--; slot = (uint8_t)((p->ev_head + p->ev_count) % FX_EVENTS); }
    snprintf(p->events[slot], FX_EVENT_LEN, "%s", text);
    p->ev_count++;
}

bool fx_pop_event(fx_device_t *p, char *out, size_t cap)
{
    if (!p || p->ev_count == 0) return false;
    snprintf(out, cap, "%s", p->events[p->ev_head]);
    p->ev_head = (uint8_t)((p->ev_head + 1) % FX_EVENTS);
    p->ev_count--;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Rows and register map                                                      */
/* ------------------------------------------------------------------------- */
static const fx_row_t *find_row(const fx_device_t *p, bool coil, uint16_t idx)
{
    const fx_type_info_t *ti = fx_type_info(p->type);
    for (uint8_t i = 0; i < ti->nrows; i++) {
        const fx_row_t *r = &ti->rows[i];
        if (((r->flags & FX_COIL) != 0) == coil && r->addr == idx) return r;
    }
    return NULL;
}

int32_t fx_reg_address(const fx_type_info_t *ti, uint8_t idx)
{
    if (!ti) return -1;
    if (ti->nregs == 0) return idx < FX_HR ? idx : -1;
    for (uint8_t i = 0; i < ti->nregs; i++) if (ti->regs[i].idx == idx) return ti->regs[i].addr;
    return -1;
}

/* Which image register (and which half of a float) answers Modbus address a. false = not mapped. */
static bool map_address(const fx_type_info_t *ti, uint16_t a, uint8_t *idx, uint8_t *fmt, bool *low_word)
{
    *low_word = false;
    *fmt = FX_FMT_INT16;
    if (ti->nregs == 0) {
        if (a >= FX_HR) return false;
        *idx = (uint8_t)a;
        return true;
    }
    for (uint8_t i = 0; i < ti->nregs; i++) {
        const fx_reg_t *r = &ti->regs[i];
        if (r->addr == a || (r->fmt == FX_FMT_FLOAT32 && (uint32_t)r->addr + 1u == a)) {
            *idx = r->idx;
            *fmt = r->fmt;
            *low_word = r->addr != a;
            return true;
        }
    }
    return false;
}

static void option_label(const char *opts, int idx, char *out, size_t cap)
{
    const char *p = opts;
    for (int i = 0; i < idx && p; i++) { p = strchr(p, '|'); if (p) p++; }
    if (!p) { snprintf(out, cap, "%d", idx); return; }
    size_t n = strcspn(p, "|");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void format_row(const fx_row_t *r, int16_t v, char *out, size_t cap)
{
    if (r->flags & FX_COIL) { snprintf(out, cap, "%s", v ? "ON" : "OFF"); return; }
    if (r->flags & FX_HEX) { snprintf(out, cap, "16#%04X", (unsigned)(uint16_t)v); return; }
    if (r->options) { option_label(r->options, v, out, cap); return; }
    int neg = v < 0;
    int a = neg ? -(int)v : v;
    const char *sp = r->unit[0] ? " " : "";
    if (r->decimals == 0)      snprintf(out, cap, "%d%s%s", (int)v, sp, r->unit);
    else if (r->decimals == 1) snprintf(out, cap, "%s%d.%d%s%s", neg ? "-" : "", a / 10, a % 10, sp, r->unit);
    else                       snprintf(out, cap, "%s%d.%02d%s%s", neg ? "-" : "", a / 100, a % 100, sp, r->unit);
}

int16_t fx_row_value(const fx_device_t *p, uint8_t row)
{
    const fx_type_info_t *ti = fx_type_info(p->type);
    if (!ti || row >= ti->nrows) return 0;
    const fx_row_t *r = &ti->rows[row];
    return (r->flags & FX_COIL) ? (int16_t)p->coils[r->addr] : p->hr[r->addr];
}

void fx_row_text(const fx_device_t *p, uint8_t row, char *out, size_t cap)
{
    const fx_type_info_t *ti = fx_type_info(p->type);
    if (!ti || row >= ti->nrows) { snprintf(out, cap, "-"); return; }
    format_row(&ti->rows[row], fx_row_value(p, row), out, cap);
}

void fx_nudge(fx_device_t *p, uint8_t row, int dir)
{
    const fx_type_info_t *ti = fx_type_info(p->type);
    if (!ti || row >= ti->nrows || dir == 0) return;
    const fx_row_t *r = &ti->rows[row];
    if ((r->flags & FX_COIL) || r->step == 0 || !(r->flags & FX_LAB)) return;
    int32_t d = dir > 0 ? r->step : -r->step;
    if (r->flags & FX_DERIVED) p->offset[r->addr] = (int16_t)clamp32(p->offset[r->addr] + d, -3000, 3000);
    p->hr[r->addr] = (int16_t)clamp32(p->hr[r->addr] + d, r->min, r->max);
}

void fx_toggle_coil(fx_device_t *p, uint8_t row)
{
    const fx_type_info_t *ti = fx_type_info(p->type);
    if (!ti || row >= ti->nrows || !(ti->rows[row].flags & FX_COIL)) return;
    uint8_t a = ti->rows[row].addr;
    p->coils[a] = (uint8_t)!p->coils[a];
}

/* ------------------------------------------------------------------------- */
/* Process models                                                             */
/* ------------------------------------------------------------------------- */
static void altivar_status(fx_device_t *p);

void fx_init(fx_device_t *p, fx_type_t t)
{
    memset(p, 0, sizeof(*p));
    p->type = t;
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    switch (t) {
        case FX_TANK:       h[0] = 720; h[1] = 1450; h[2] = 65; h[3] = 130; h[4] = 56; h[5] = 600; h[6] = 0; c[0] = 1; break;
        case FX_ATV340:
        case FX_ATV630:     /* powered up and running forward at 30 Hz under network control */
            h[AV_CMD] = 0x000F; h[AV_LFR] = 300; h[AV_ACC] = 50; h[AV_DEC] = 50; h[AV_RFR] = 300; h[AV_FRH] = 300;
            h[AV_LCR] = 14; p->st[0] = DS_RUNNING; altivar_status(p); break;
        case FX_CONVEYOR:   h[0] = 300; h[1] = 300; h[2] = 0; h[3] = 45; h[4] = 30; c[0] = 1; break;
        case FX_BOILER:     h[0] = 780; h[1] = 800; h[2] = 40; h[3] = 293; h[4] = 55; h[5] = 1; h[6] = 30; c[0] = 1; c[1] = 1; p->st[1] = 200; break;
        case FX_PUMPS:      h[0] = 400; h[1] = 400; h[2] = 0; h[5] = 2; h[6] = 60; h[7] = 0; h[8] = 50; break;
        case FX_COMPRESSOR: h[0] = 700; h[1] = 650; h[2] = 750; h[3] = 650; h[5] = 1; h[6] = 50; c[0] = 1; p->st[1] = 650; break;
        case FX_AHU:        h[0] = 210; h[1] = 200; h[2] = 230; h[3] = 70; h[4] = 70; h[5] = 3; h[6] = 40; h[7] = 120; h[8] = 650; c[0] = 1; break;
        case FX_VFD:        h[0] = 1200; h[1] = 1200; h[2] = 82; h[3] = 55; h[4] = 540; c[0] = 1; break;
        case FX_METER:      h[0] = 512; h[1] = 498; h[2] = 505; h[3] = 4152; h[4] = 4148; h[5] = 4155; h[8] = 5000;
                             h[9] = 1250; break;
        default:             h[0] = 11; h[1] = 22; h[2] = 33; h[3] = 44; break;
    }
}

void fx_set_stopped(fx_device_t *p, bool stopped)
{
    if (p->stopped == stopped) return;
    p->stopped = stopped;
    event(p, stopped ? "PLC switched to STOP: the program no longer scans" : "PLC switched to RUN");
}

/* Move v toward target by at most up / down per step */
static int32_t ramp(int32_t v, int32_t target, int32_t up, int32_t down)
{
    return v + clamp32(target - v, -down, up);
}

static int32_t isqrt(int32_t v)
{
    if (v <= 0) return 0;
    int32_t r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

/* Integrate delta (in 1/scale units) into a register through a remainder */
static void integrate(int16_t *reg, int32_t *rem, int32_t delta, int32_t scale, int32_t lo, int32_t hi)
{
    *rem += delta;
    int32_t whole = *rem / scale;
    *rem -= whole * scale;
    int32_t v = *reg + whole;
    if (v < lo || v > hi) *rem = 0;                 /* clamped: drop the remainder (not when merely at a limit) */
    *reg = (int16_t)clamp32(v, lo, hi);
}

static void step_tank(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[3]) { c[4] = 0; c[3] = 0; }                               /* fault reset clears the overload trip */
    /* HMI heartbeat watchdog (%MW7): 5 s without a change after the HMI has been seen once sets the
     * stop latch. It stays set until an operator resets it from a working HMI: nothing restarts by itself. */
    if (h[7] != (int16_t)p->st[2]) { p->st[2] = h[7]; p->st[3] = 0; p->st[4] = 0; }
    else if (h[7] != 0 && p->st[3] < 1000) p->st[3]++;
    if (h[7] != 0 && p->st[3] >= 50 && !p->st[4]) {
        p->st[4] = 1;
        c[2] = 1;
        event(p, "HMI heartbeat lost for 5 s: stop latch set");
    }
    if (c[2]) { c[0] = 0; c[1] = 0; }                               /* stop latch set: pumps off */
    if (c[4]) c[0] = 0;                                             /* overload trips pump A */
    if (h[6] == 1 && !c[2] && !c[4]) {                              /* Auto: hold the level at its setpoint */
        if (h[0] < h[5] - 20) c[0] = 1;
        if (h[0] > h[5] + 20) c[0] = 0;
    }
    int32_t rpm = ramp(h[1], c[0] ? 1450 : 0, 60, 150);
    h[1] = (int16_t)rpm;
    integrate(&h[0], &p->st[0], 60 * rpm / 1450 - h[2] * 9 / 10, 100, 0, 1000);
    h[3] = (int16_t)clamp32(20 + rpm * 95 / 1450 + (100 - h[2]) * 40 / 100 + p->offset[3], -32000, 32000);
    h[4] = (int16_t)clamp32(30 + rpm * 29 / 1450 - (c[1] ? 14 : 0) + p->offset[4], -32000, 32000);
}

/* --- Altivar: CiA402 state machine driven by the control word CMD (8501) --- */
static void altivar_status(fx_device_t *p)
{
    static const uint16_t eta[] = { 0x0050, 0x0031, 0x0033, 0x0037, 0x0008 };   /* incl. bit 4 voltage present */
    int32_t st = clamp32(p->st[0], DS_SWITCH_ON_DISABLED, DS_FAULT);
    uint16_t w = eta[st];
    if (!p->coils[AV_C_LOCAL]) w |= 0x0200;                                 /* bit 9: network has control */
    if (p->hr[AV_RFR] < 0 || (st == DS_RUNNING && (p->hr[AV_CMD] & 0x0800))) w |= 0x8000;  /* bit 15: reverse */
    if (st == DS_RUNNING && p->hr[AV_RFR] == p->hr[AV_FRH]) w |= 0x0400;   /* bit 10: reference reached */
    p->hr[AV_ETA] = (int16_t)w;
}

static void altivar_fault(fx_device_t *p, const char *why)
{
    if (p->st[0] == DS_FAULT) return;
    p->st[0] = DS_FAULT;
    p->hr[AV_LFT] = AV_FAULT_CODE;
    char m[FX_EVENT_LEN];
    snprintf(m, sizeof(m), "Drive tripped: %s (fault code %d)", why, AV_FAULT_CODE);
    event(p, m);
}

/* A write to CMD: the transitions of the drive state machine */
static void altivar_command(fx_device_t *p, uint16_t old_cmd, uint16_t cmd)
{
    static const char * const names[] = { "switch on disabled", "ready", "switched on", "running", "fault" };
    int32_t before = p->st[0];
    if (p->coils[AV_C_LOCAL]) {
        event(p, "Command ignored: the drive is in local (keypad) control");
        return;
    }
    if (p->st[0] == DS_FAULT) {
        if ((cmd & 0x0080) && !(old_cmd & 0x0080)) {                         /* bit 7 rising edge: fault reset */
            if (p->coils[AV_C_OVERLOAD]) event(p, "Fault reset refused: the overload is still present");
            else { p->st[0] = DS_SWITCH_ON_DISABLED; event(p, "HMI reset the drive fault"); }
        }
    } else if (!(cmd & 0x0002) || !(cmd & 0x0004)) {
        p->st[0] = DS_SWITCH_ON_DISABLED;                                    /* disable voltage / quick stop */
    } else if ((cmd & 0x000F) == 0x0006) {
        p->st[0] = DS_READY;                                                 /* shutdown */
    } else if ((cmd & 0x000F) == 0x0007) {
        if (p->st[0] == DS_READY || p->st[0] == DS_RUNNING) p->st[0] = DS_SWITCHED_ON;   /* switch on / ramp stop */
    } else if ((cmd & 0x000F) == 0x000F) {
        if (p->st[0] != DS_SWITCH_ON_DISABLED) p->st[0] = DS_RUNNING;        /* enable operation */
    }
    if (p->st[0] != before) {
        char m[FX_EVENT_LEN];
        if (p->st[0] == DS_RUNNING) snprintf(m, sizeof(m), "Drive running %s", (cmd & 0x0800) ? "in reverse" : "forward");
        else snprintf(m, sizeof(m), "Drive state: %s", names[clamp32(p->st[0], 0, 4)]);
        event(p, m);
    }
    altivar_status(p);
}

static void step_altivar(fx_device_t *p)
{
    int16_t *h = p->hr;
    if (p->coils[AV_C_OVERLOAD]) altivar_fault(p, "motor overload");
    bool run = p->st[0] == DS_RUNNING;
    int32_t target = run ? ((h[AV_CMD] & 0x0800) ? -h[AV_LFR] : h[AV_LFR]) : 0;
    int32_t f = h[AV_RFR];
    if (p->st[0] == DS_FAULT || p->st[0] == DS_SWITCH_ON_DISABLED) {
        f = ramp(f, 0, 60, 60);                                              /* freewheel */
    } else {
        int32_t up = clamp32(500 / (h[AV_ACC] > 0 ? h[AV_ACC] : 1), 1, 500);  /* ACC = 0 -> 50 Hz time */
        int32_t dn = clamp32(500 / (h[AV_DEC] > 0 ? h[AV_DEC] : 1), 1, 500);
        bool accel = iabs(target) > iabs(f) && (target >= 0) == (f >= 0);
        f = accel ? ramp(f, target, up, up) : ramp(f, target, dn, dn);
    }
    h[AV_RFR] = (int16_t)f;
    h[AV_FRH] = (int16_t)target;
    int32_t amps = (f || run) ? 8 + iabs(f) * 10 / 500 : 0;                  /* 0.8 A idle .. 1.8 A at 50 Hz */
    h[AV_LCR] = (int16_t)clamp32(amps + p->offset[AV_LCR], 0, 100);
    if (h[AV_LCR] > 40) altivar_fault(p, "over-current");                    /* more than 4.0 A */
    altivar_status(p);
}

static void step_conveyor(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[3]) { c[1] = 0; c[3] = 0; }
    if (c[4]) { h[2] = 0; p->st[0] = 0; c[4] = 0; }
    if (c[1] || c[2]) c[0] = 0;                                     /* jam or pull-cord stops the belt */
    int32_t v = c[1] ? 0 : ramp(h[0], c[0] ? h[1] : 0, 20, 40);
    h[0] = (int16_t)v;
    int16_t count = h[2];
    integrate(&count, &p->st[0], v, 6000, 0, 32000);               /* items = speed / 10 per minute */
    h[2] = count;
    h[4] = (int16_t)(v / 10);
    h[3] = (int16_t)clamp32((v ? 15 + v / 10 : 0) + p->offset[3], 0, 32000);
}

static void step_boiler(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (h[4] < 20) c[2] = 1;                                        /* low water trip latches */
    if (c[4]) { if (h[4] >= 20) c[2] = 0; c[4] = 0; }
    if (c[2] || c[3]) c[0] = 0;
    int32_t fire;
    if (!c[0]) fire = 0;
    else if (h[5] == 1) {                                           /* Auto: PI temperature control */
        p->st[1] = clamp32(p->st[1] + (h[1] - h[0]) / 8, 0, 1000);  /* integral, 0.1 % units */
        fire = clamp32(p->st[1] / 10 + (h[1] - h[0]) / 2, 0, 100);
    } else fire = h[6];
    h[2] = (int16_t)fire;
    c[1] = fire > 0;
    integrate(&h[0], &p->st[0], fire * 50 - (h[0] - 150) * 100 / 30, 100, 0, 1200);   /* 0.1 C */
    h[3] = (int16_t)clamp32(100 + (h[0] - 200) / 3 + p->offset[3], -32000, 32000);
    integrate(&h[4], &p->st[2], (55 - h[4]) * 4 - fire / 2, 100, 0, 100);   /* feed water vs. evaporation */
}

static void step_pumps(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[4]) { c[2] = 0; c[3] = 0; c[4] = 0; }
    int duty = h[7] ? 1 : 0;
    int active = c[2 + duty] ? (c[2 + (1 - duty)] ? -1 : 1 - duty) : duty;   /* standby takes over on a fault */
    int32_t target = 0;
    if (active >= 0) {
        if (h[5] == 1) target = h[6];
        else if (h[5] == 2) {                                       /* Auto: feed-forward + integral trim */
            p->st[0] = clamp32(p->st[0] + (h[1] - h[0]) / 20, -300, 300);
            target = clamp32(isqrt((h[1] + h[8] * 2) * 100 / 9) + p->st[0] / 10, 0, 100);
        }
    }
    if (h[5] != 2) p->st[0] = 0;
    for (int k = 0; k < 2; k++) {
        int32_t s = ramp(h[3 + k], k == active ? target : 0, 5, 10);
        h[3 + k] = (int16_t)s;
        c[k] = s > 0;
    }
    int32_t s = h[3] > h[4] ? h[3] : h[4];
    h[0] = (int16_t)clamp32(s * s * 9 / 100 - h[8] * 2 + p->offset[0], 0, 1200);
    h[2] = (int16_t)(s * h[8] / 5);
}

static void step_compressor(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (h[3] > 1100) c[1] = 1;                                      /* discharge over 110 C trips */
    if (c[2]) { if (h[3] < 1000) c[1] = 0; c[2] = 0; }
    if (c[1]) c[0] = 0;
    if (!c[0]) h[5] = 0;
    else if (h[5] == 0 || h[0] <= h[1]) h[5] = 1;                   /* below load pressure: load */
    else if (h[0] >= h[2]) h[5] = 2;                                /* above unload pressure: unload */
    integrate(&h[0], &p->st[0], (h[5] == 1 ? 120 : 0) - h[6], 100, 0, 1200);   /* 0.01 bar */
    int32_t tt = h[5] == 1 ? 850 : h[5] == 2 ? 650 : 250;                       /* discharge temp, 0.1 C */
    p->st[1] += (tt - p->st[1]) / 30 + ((tt - p->st[1]) > 0 ? 1 : (tt - p->st[1]) < 0 ? -1 : 0);
    h[3] = (int16_t)clamp32(p->st[1] + p->offset[3], -32000, 32000);
    h[4] = (int16_t)(h[5] == 1 ? 450 : h[5] == 2 ? 180 : 0);
}

static void step_ahu(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[2]) c[0] = 0;                                             /* fire alarm: fan off */
    int32_t fan = ramp(h[3], (c[0] && h[5] != 0) ? h[4] : 0, 2, 4);
    h[3] = (int16_t)fan;
    h[6] = (int16_t)(fan ? clamp32(20 + (h[8] - 400) / 16, 20, 100) : 0);
    bool heat = h[5] == 1 || h[5] == 3, cool = h[5] == 2 || h[5] == 3;
    int32_t t = h[0];
    if (fan && heat && t < h[1]) t += 2;
    else if (fan && cool && t > h[1]) t -= 2;
    else t += (h[2] - t) / 50;
    h[0] = (int16_t)clamp32(t, -100, 500);
    c[1] = h[7] > 250;
}

static void step_vfd(fx_device_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[2]) { c[1] = 0; c[2] = 0; }
    if (c[1]) c[0] = 0;
    int32_t n = c[1] ? ramp(h[0], 0, 0, 150) : ramp(h[0], c[0] ? h[1] : 0, 50, 50);
    h[0] = (int16_t)n;
    h[3] = (int16_t)(n ? 35 + n / 60 : 0);
    h[2] = (int16_t)clamp32((n ? 30 + h[3] * 2 : 0) + p->offset[2], 0, 32000);
    h[4] = (int16_t)(n ? 535 : 540);
    h[5] = (int16_t)(c[1] ? 2310 : 0);
}

/* Three-phase meter: power and energy follow the currents and voltages the user sets */
static void step_meter(fx_device_t *p)
{
    int16_t *h = p->hr;
    int32_t vll = (h[3] + h[4] + h[5]) / 3;                                  /* 0.1 V */
    int32_t iavg = (h[0] + h[1] + h[2]) / 3;                                 /* 0.01 A */
    h[6] = (int16_t)vll;
    /* P = sqrt(3) x V x I x PF(0.9); kW in 0.01 units: vll/10 * iavg/100 * 1.5588 / 1000 * 100 */
    int32_t kw = vll * iavg / 6415;
    h[7] = (int16_t)clamp32(kw + p->offset[7], 0, 32000);
    integrate(&h[9], &p->st[0], h[7], 3600 * 10 * 100, 0, 32000);          /* kWh: 0.01 kW for 0.1 s */
}

void fx_step(fx_device_t *p)
{
    if (p->stopped) return;
    p->scans++;
    switch (p->type) {
        case FX_TANK:       step_tank(p); break;
        case FX_ATV340:
        case FX_ATV630:     step_altivar(p); break;
        case FX_CONVEYOR:   step_conveyor(p); break;
        case FX_BOILER:     step_boiler(p); break;
        case FX_PUMPS:      step_pumps(p); break;
        case FX_COMPRESSOR: step_compressor(p); break;
        case FX_AHU:        step_ahu(p); break;
        case FX_VFD:        step_vfd(p); break;
        case FX_METER:      step_meter(p); break;
        default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* Modbus TCP server logic                                                    */
/* ------------------------------------------------------------------------- */
static uint16_t be16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }

int32_t fx_frame_length(const uint8_t *buf, size_t len)
{
    if (len < 7) return 0;
    uint16_t proto = be16(buf + 2), plen = be16(buf + 4);
    if (proto != 0 || plen < 2 || plen > FX_MAX_ADU - 6) return -1;
    return (size_t)plen + 6u <= len ? (int32_t)plen + 6 : 0;
}

static size_t reply(uint8_t *out, size_t cap, uint16_t tid, uint8_t unit, const uint8_t *pdu, size_t n)
{
    if (n + 7 > cap) return 0;
    put16(out, tid); put16(out + 2, 0); put16(out + 4, (uint16_t)(n + 1));
    out[6] = unit;
    memcpy(out + 7, pdu, n);
    return n + 7;
}

static size_t exception(fx_device_t *p, uint8_t *out, size_t cap, uint16_t tid, uint8_t unit, uint8_t fc, uint8_t code)
{
    uint8_t pdu[2] = { (uint8_t)(fc | 0x80), code };
    p->exceptions++;
    return reply(out, cap, tid, unit, pdu, 2);
}

/* First coil in mask that is set, for the refusal message */
static const fx_row_t *blocker(const fx_device_t *p, uint8_t mask)
{
    for (uint8_t b = 0; b < FX_COILS; b++)
        if ((mask & (1u << b)) && p->coils[b]) return find_row(p, true, b);
    return NULL;
}

/* Register value seen on the wire for address a */
static uint16_t wire_value(const fx_device_t *p, uint8_t idx, uint8_t fmt, bool low_word)
{
    if (fmt != FX_FMT_FLOAT32) return (uint16_t)p->hr[idx];
    const fx_row_t *row = find_row(p, false, idx);
    float f = (float)p->hr[idx];
    for (uint8_t d = 0; row && d < row->decimals; d++) f /= 10.0f;
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return low_word ? (uint16_t)(u & 0xFFFFu) : (uint16_t)(u >> 16);
}

/* FC43 / MEI 0x0E: basic objects 0-2, plus 0x06 UserApplicationName in the regular category */
static size_t device_id(fx_device_t *p, const fx_type_info_t *ti, const uint8_t *pdu, size_t plen,
                        uint8_t *out, size_t cap, uint16_t tid, uint8_t unit)
{
    if (plen < 4 || pdu[1] != 0x0E || !ti->vendor) return exception(p, out, cap, tid, unit, 0x2B, 0x01);
    uint8_t code = pdu[2];
    if (code < 1 || code > 2) return exception(p, out, cap, tid, unit, 0x2B, 0x03);
    bool regular = code == 2 && ti->app;
    uint8_t r[4 * (2 + 32) + 7];
    size_t n = 0;
    uint8_t nobj = regular ? 4 : 3;
    r[n++] = 0x2B; r[n++] = 0x0E; r[n++] = code;
    r[n++] = (uint8_t)(ti->app ? 0x02 : 0x01);          /* conformity level: regular / basic, stream access */
    r[n++] = 0x00; r[n++] = 0x00; r[n++] = nobj;
    const char *obj[4] = { ti->vendor, ti->product, ti->revision, ti->app };
    static const uint8_t ids[4] = { 0x00, 0x01, 0x02, 0x06 };
    for (uint8_t i = 0; i < nobj; i++) {
        size_t sl = strlen(obj[i]);
        if (sl > 32) sl = 32;
        r[n++] = ids[i]; r[n++] = (uint8_t)sl;
        memcpy(r + n, obj[i], sl);
        n += sl;
    }
    return reply(out, cap, tid, unit, r, n);
}

size_t fx_handle(fx_device_t *p, const uint8_t *adu, size_t len, uint8_t *out, size_t cap)
{
    int32_t flen = fx_frame_length(adu, len);
    if (flen <= 0 || (size_t)flen < 8) return 0;
    uint16_t tid = be16(adu);
    uint8_t unit = adu[6];
    const uint8_t *pdu = adu + 7;
    size_t plen = (size_t)flen - 7;
    uint8_t fc = pdu[0];
    const fx_type_info_t *ti = fx_type_info(p->type);

    p->requests++;
    if (p->freeze) return 0;

    if (fc == 0x2B) return device_id(p, ti, pdu, plen, out, cap, tid, unit);
    if (p->errors) return exception(p, out, cap, tid, unit, fc, 0x04);     /* server device failure */
    if ((fc == 0x01 || fc == 0x05) && !ti->has_coils) return exception(p, out, cap, tid, unit, fc, 0x01);
    if (plen < 5) return exception(p, out, cap, tid, unit, fc, 0x03);

    uint16_t addr = be16(pdu + 1), qv = be16(pdu + 3);
    char msg[FX_EVENT_LEN], val[32];
    if (fc == 0x03) {
        if (qv == 0 || qv > 125 || (uint32_t)addr + qv > 65536u) return exception(p, out, cap, tid, unit, fc, 0x03);
        uint8_t r[2 + 2 * 125];
        size_t n = 0;
        r[n++] = fc;
        r[n++] = (uint8_t)(qv * 2);
        for (uint16_t i = 0; i < qv; i++) {
            uint8_t idx, fmt;
            bool lo;
            if (!map_address(ti, (uint16_t)(addr + i), &idx, &fmt, &lo))
                return exception(p, out, cap, tid, unit, fc, 0x02);     /* any unmapped address: illegal address */
            put16(r + n, wire_value(p, idx, fmt, lo));
            n += 2;
        }
        return reply(out, cap, tid, unit, r, n);
    }
    if (fc == 0x01) {
        if (qv == 0 || qv > 125 || (uint32_t)addr + qv > FX_COILS) return exception(p, out, cap, tid, unit, fc, 0x02);
        uint8_t r[2 + 16];
        size_t n = 0;
        r[n++] = fc;
        uint8_t bytes = (uint8_t)((qv + 7) / 8);
        r[n++] = bytes;
        memset(r + n, 0, bytes);
        for (uint16_t i = 0; i < qv; i++) if (p->coils[addr + i]) r[n + i / 8] |= (uint8_t)(1u << (i % 8));
        n += bytes;
        return reply(out, cap, tid, unit, r, n);
    }
    if (fc == 0x05) {
        const fx_row_t *row = addr < FX_COILS ? find_row(p, true, addr) : NULL;
        if (!row || !(row->flags & FX_HMI_W)) return exception(p, out, cap, tid, unit, fc, 0x02);
        if (qv != 0xFF00 && qv != 0x0000) return exception(p, out, cap, tid, unit, fc, 0x03);
        bool on = qv == 0xFF00;
        const fx_row_t *blk = on ? blocker(p, row->block_mask) : NULL;
        if (blk) {
            snprintf(msg, sizeof(msg), "HMI tried to switch %s ON: refused, %s is active", row->label, blk->label);
            event(p, msg);                                            /* interlock: the PLC keeps it off */
        } else {
            p->coils[addr] = on ? 1 : 0;
            if (row->flags & FX_PULSE) { if (on) { snprintf(msg, sizeof(msg), "HMI pressed %s", row->label); event(p, msg); } }
            else { snprintf(msg, sizeof(msg), "HMI switched %s %s", row->label, on ? "ON" : "OFF"); event(p, msg); }
        }
        p->writes++;
        return reply(out, cap, tid, unit, pdu, 5);
    }
    if (fc == 0x06) {
        uint8_t idx, fmt;
        bool lo;
        const fx_row_t *row = map_address(ti, addr, &idx, &fmt, &lo) && fmt == FX_FMT_INT16 ? find_row(p, false, idx) : NULL;
        if (!row || !(row->flags & FX_HMI_W)) return exception(p, out, cap, tid, unit, fc, 0x02);
        int32_t v = (int16_t)qv;
        if (v < row->min || v > row->max) return exception(p, out, cap, tid, unit, fc, 0x03);   /* out of range */
        int16_t old = p->hr[idx];
        p->hr[idx] = (int16_t)v;
        p->writes++;
        if (is_altivar(p->type) && idx == AV_CMD) {
            snprintf(msg, sizeof(msg), "HMI wrote CMD = 16#%04X", (unsigned)qv);
            event(p, msg);
            altivar_command(p, (uint16_t)old, qv);
        } else if (strcmp(row->label, "HMI heartbeat") != 0) {   /* once a second: only its loss is news */
            format_row(row, (int16_t)v, val, sizeof(val));
            snprintf(msg, sizeof(msg), "HMI set %s to %s", row->label, val);
            event(p, msg);
        }
        return reply(out, cap, tid, unit, pdu, 5);
    }
    return exception(p, out, cap, tid, unit, fc, 0x01);
}
