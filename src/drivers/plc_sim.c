/**
 * @file plc_sim.c
 * @brief Simulated Modbus devices: process models and the Modbus server (FC01/03/05/06/43)
 */

#include "../../include/drivers/plc_sim.h"
#include <stdio.h>
#include <string.h>

/* Row constructors: register, enum register, coil */
#define R(l, u, f, a, d, s, mn, mx)  { l, u, (uint8_t)(f), a, d, s, mn, mx, 0, NULL }
#define E(l, f, a, mx, o)            { l, "", (uint8_t)(f), a, 0, 1, 0, mx, 0, o }
#define C(l, f, a, blk)              { l, "", (uint8_t)((f) | SIM_COIL), a, 0, 0, 0, 1, (uint8_t)(blk), NULL }
#define BIT(n)                       (1u << (n))
#define HW  SIM_HMI_W
#define LB  SIM_LAB
#define DV  SIM_DERIVED
#define PL  SIM_PULSE

static const sim_type_info_t s_types[SIM_TYPE_COUNT] = {
    [SIM_TANK] = { "Tank skid", "Modicon M340", "Schneider Electric", "BMX P34 2020", "V3.20", true, 12, {
        R("Tank level",     "%",   LB,      0, 1, 50, 0, 1000),
        R("Pump A speed",   "rpm", DV,      1, 0, 0,  0, 3000),
        R("Valve V-101",    "%",   HW | LB, 2, 0, 5,  0, 100),
        R("Line pressure",  "kPa", DV | LB, 3, 0, 10, -200, 400),
        R("Coolant temp",   "C",   DV | LB, 4, 0, 5,  -50, 150),
        R("Level setpoint", "%",   HW | LB, 5, 1, 50, 100, 950),
        E("Control mode",          HW | LB, 6, 1, "Manual|Auto"),
        C("Feed pump A",           HW | LB, 0, BIT(2) | BIT(4)),
        C("Coolant pump B",        HW | LB, 1, BIT(2)),
        C("E-stop",                HW | LB, 2, 0),
        C("Fault reset",           HW | LB | PL, 3, 0),
        C("Pump A overload",       LB, 4, 0) } },
    [SIM_DRIVE] = { "Drive", "Altivar ATV320", "Schneider Electric", "ATV320U15N4C", "V2.6", true, 10, {
        R("Output frequency", "Hz", DV,      0, 1, 0,  0, 600),
        R("Motor current",    "A",  DV | LB, 1, 1, 20, -200, 500),
        R("Speed setpoint",   "Hz", HW | LB, 2, 1, 50, 0, 600),
        R("Drive temp",       "C",  DV | LB, 3, 0, 5,  -50, 150),
        R("Accel time",       "s",  HW | LB, 4, 1, 10, 1, 600),
        E("Direction",              HW | LB, 5, 1, "Forward|Reverse"),
        R("Motor power",      "kW", DV,      6, 1, 0,  0, 500),
        C("Run",                    HW | LB, 0, BIT(1)),
        C("Drive fault",            LB, 1, 0),
        C("Fault reset",            HW | LB | PL, 2, 0) } },
    [SIM_CONVEYOR] = { "Conveyor", "Modicon M241", "Schneider Electric", "TM241CEC24T", "V5.1", true, 10, {
        R("Belt speed",     "m/min", DV,      0, 1, 0,  0, 1500),
        R("Speed setpoint", "m/min", HW | LB, 1, 1, 50, 0, 1200),
        R("Item count",     "",      DV,      2, 0, 0,  0, 32000),
        R("Motor current",  "A",     DV | LB, 3, 1, 10, -100, 300),
        R("Throughput",     "/min",  DV,      4, 0, 0,  0, 2000),
        C("Run",                     HW | LB, 0, BIT(1) | BIT(2)),
        C("Jam detected",            LB, 1, 0),
        C("Pull-cord stop",          HW | LB, 2, 0),
        C("Jam reset",               HW | LB | PL, 3, 0),
        C("Counter reset",           HW | LB | PL, 4, 0) } },
    [SIM_BOILER] = { "Boiler", "Modicon M580", "Schneider Electric", "BME P58 2040", "V4.10", true, 12, {
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
        C("E-stop",                 HW | LB, 3, 0),
        C("Trip reset",             HW | LB | PL, 4, 0) } },
    [SIM_PUMPS] = { "Pump station", "Siemens S7-1200", "Siemens", "6ES7 214-1AG40-0XB0", "V4.6", true, 14, {
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
        C("Fault reset",                HW | LB | PL, 4, 0) } },
    [SIM_COMPRESSOR] = { "Air compressor", "Allen-Bradley Micro850", "Rockwell Automation", "2080-LC50-24QWB", "21.011", true, 10, {
        R("Air pressure",    "bar", LB,      0, 2, 20, 0, 1200),
        R("Load pressure",   "bar", HW | LB, 1, 2, 25, 400, 900),
        R("Unload pressure", "bar", HW | LB, 2, 2, 25, 450, 1000),
        R("Discharge temp",  "C",   DV | LB, 3, 1, 50, -200, 1500),
        R("Motor current",   "A",   DV,      4, 1, 0,  0, 1000),
        E("State",                  DV,      5, 2, "Stopped|Loaded|Unloaded"),
        R("Air demand",      "%",   LB,      6, 0, 10, 0, 100),
        C("Run",                    HW | LB, 0, BIT(1)),
        C("High temp trip",         LB, 1, 0),
        C("Trip reset",             HW | LB | PL, 2, 0) } },
    [SIM_AHU] = { "Air handling unit", "Modicon M251", "Schneider Electric", "TM251MESE", "V5.0", true, 12, {
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
        C("Fire alarm",                LB, 2, 0) } },
    [SIM_VFD] = { "ABB drive", "ABB ACS580", "ABB", "ACS580-01-12A7-4", "2.15", true, 9, {
        R("Motor speed",     "rpm", DV,      0, 0, 0,   0, 3000),
        R("Speed reference", "rpm", HW | LB, 1, 0, 100, 0, 1500),
        R("Motor current",   "A",   DV | LB, 2, 1, 10,  -200, 500),
        R("Torque",          "%",   DV,      3, 0, 0,   0, 300),
        R("DC bus",          "V",   DV,      4, 0, 0,   0, 900),
        R("Fault code",      "",    DV,      5, 0, 0,   0, 32000),
        C("Run",                    HW | LB, 0, BIT(1)),
        C("Fault",                  LB, 1, 0),
        C("Fault reset",            HW | LB | PL, 2, 0) } },
    [SIM_METER] = { "Power meter", "PowerLogic PM5560", "Schneider Electric", "PM5560", "V2.7.4", false, 4, {
        R("Voltage L1", "V",  LB, 0, 1, 50, 0, 5000),
        R("Voltage L2", "V",  LB, 1, 1, 50, 0, 5000),
        R("Voltage L3", "V",  LB, 2, 1, 50, 0, 5000),
        R("Frequency",  "Hz", LB, 3, 2, 10, 0, 7000) } },
    [SIM_ANON] = { "Modbus device", "Unknown make", NULL, NULL, NULL, false, 4, {
        R("Register 0", "", LB, 0, 0, 10, -1000, 1000),
        R("Register 1", "", LB, 1, 0, 10, -1000, 1000),
        R("Register 2", "", LB, 2, 0, 10, -1000, 1000),
        R("Register 3", "", LB, 3, 0, 10, -1000, 1000) } },
};

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

const sim_type_info_t *sim_type_info(sim_type_t t)
{
    return (unsigned)t < SIM_TYPE_COUNT ? &s_types[t] : NULL;
}

/* ------------------------------------------------------------------------- */
/* Events (what the HMI did) for the lab's activity log                       */
/* ------------------------------------------------------------------------- */
static void event(sim_plc_t *p, const char *text)
{
    uint8_t slot = (uint8_t)((p->ev_head + p->ev_count) % SIM_EVENTS);
    if (p->ev_count == SIM_EVENTS) { p->ev_head = (uint8_t)((p->ev_head + 1) % SIM_EVENTS); p->ev_count--; slot = (uint8_t)((p->ev_head + p->ev_count) % SIM_EVENTS); }
    snprintf(p->events[slot], SIM_EVENT_LEN, "%s", text);
    p->ev_count++;
}

bool sim_pop_event(sim_plc_t *p, char *out, size_t cap)
{
    if (!p || p->ev_count == 0) return false;
    snprintf(out, cap, "%s", p->events[p->ev_head]);
    p->ev_head = (uint8_t)((p->ev_head + 1) % SIM_EVENTS);
    p->ev_count--;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Rows                                                                       */
/* ------------------------------------------------------------------------- */
static const sim_row_t *find_row(const sim_plc_t *p, bool coil, uint16_t addr)
{
    const sim_type_info_t *ti = sim_type_info(p->type);
    for (uint8_t i = 0; i < ti->nrows; i++) {
        const sim_row_t *r = &ti->rows[i];
        if (((r->flags & SIM_COIL) != 0) == coil && r->addr == addr) return r;
    }
    return NULL;
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

static void format_row(const sim_row_t *r, int16_t v, char *out, size_t cap)
{
    if (r->flags & SIM_COIL) { snprintf(out, cap, "%s", v ? "ON" : "OFF"); return; }
    if (r->options) { option_label(r->options, v, out, cap); return; }
    int neg = v < 0;
    int a = neg ? -(int)v : v;
    const char *sp = r->unit[0] ? " " : "";
    if (r->decimals == 0)      snprintf(out, cap, "%d%s%s", (int)v, sp, r->unit);
    else if (r->decimals == 1) snprintf(out, cap, "%s%d.%d%s%s", neg ? "-" : "", a / 10, a % 10, sp, r->unit);
    else                       snprintf(out, cap, "%s%d.%02d%s%s", neg ? "-" : "", a / 100, a % 100, sp, r->unit);
}

int16_t sim_row_value(const sim_plc_t *p, uint8_t row)
{
    const sim_type_info_t *ti = sim_type_info(p->type);
    if (!ti || row >= ti->nrows) return 0;
    const sim_row_t *r = &ti->rows[row];
    return (r->flags & SIM_COIL) ? (int16_t)p->coils[r->addr] : p->hr[r->addr];
}

void sim_row_text(const sim_plc_t *p, uint8_t row, char *out, size_t cap)
{
    const sim_type_info_t *ti = sim_type_info(p->type);
    if (!ti || row >= ti->nrows) { snprintf(out, cap, "-"); return; }
    format_row(&ti->rows[row], sim_row_value(p, row), out, cap);
}

void sim_nudge(sim_plc_t *p, uint8_t row, int dir)
{
    const sim_type_info_t *ti = sim_type_info(p->type);
    if (!ti || row >= ti->nrows || dir == 0) return;
    const sim_row_t *r = &ti->rows[row];
    if ((r->flags & SIM_COIL) || r->step == 0 || !(r->flags & SIM_LAB)) return;
    int32_t d = dir > 0 ? r->step : -r->step;
    if (r->flags & SIM_DERIVED) p->offset[r->addr] = (int16_t)clamp32(p->offset[r->addr] + d, -3000, 3000);
    p->hr[r->addr] = (int16_t)clamp32(p->hr[r->addr] + d, r->min, r->max);
}

void sim_toggle_coil(sim_plc_t *p, uint8_t row)
{
    const sim_type_info_t *ti = sim_type_info(p->type);
    if (!ti || row >= ti->nrows || !(ti->rows[row].flags & SIM_COIL)) return;
    uint8_t a = ti->rows[row].addr;
    p->coils[a] = (uint8_t)!p->coils[a];
}

/* ------------------------------------------------------------------------- */
/* Process models                                                             */
/* ------------------------------------------------------------------------- */
void sim_init(sim_plc_t *p, sim_type_t t)
{
    memset(p, 0, sizeof(*p));
    p->type = t;
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    switch (t) {
        case SIM_TANK:       h[0] = 720; h[1] = 1450; h[2] = 65; h[3] = 130; h[4] = 56; h[5] = 600; h[6] = 0; c[0] = 1; break;
        case SIM_DRIVE:      h[0] = 500; h[1] = 170; h[2] = 500; h[3] = 50; h[4] = 50; h[5] = 0; h[6] = 17; c[0] = 1; p->st[0] = 0; break;
        case SIM_CONVEYOR:   h[0] = 300; h[1] = 300; h[2] = 0; h[3] = 45; h[4] = 30; c[0] = 1; break;
        case SIM_BOILER:     h[0] = 780; h[1] = 800; h[2] = 40; h[3] = 293; h[4] = 55; h[5] = 1; h[6] = 30; c[0] = 1; c[1] = 1; p->st[1] = 200; break;
        case SIM_PUMPS:      h[0] = 400; h[1] = 400; h[2] = 0; h[5] = 2; h[6] = 60; h[7] = 0; h[8] = 50; break;
        case SIM_COMPRESSOR: h[0] = 700; h[1] = 650; h[2] = 750; h[3] = 650; h[5] = 1; h[6] = 50; c[0] = 1; p->st[1] = 650; break;
        case SIM_AHU:        h[0] = 210; h[1] = 200; h[2] = 230; h[3] = 70; h[4] = 70; h[5] = 3; h[6] = 40; h[7] = 120; h[8] = 650; c[0] = 1; break;
        case SIM_VFD:        h[0] = 1200; h[1] = 1200; h[2] = 82; h[3] = 55; h[4] = 540; c[0] = 1; break;
        case SIM_METER:      h[0] = 2301; h[1] = 2298; h[2] = 2305; h[3] = 4999; break;
        default:             h[0] = 11; h[1] = 22; h[2] = 33; h[3] = 44; break;
    }
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

static void step_tank(sim_plc_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[3]) { c[4] = 0; c[3] = 0; }                               /* fault reset clears the overload trip */
    if (c[2]) { c[0] = 0; c[1] = 0; }                               /* E-stop latched: pumps off */
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

static void step_drive(sim_plc_t *p)
{
    int16_t *h = p->hr;
    uint8_t *c = p->coils;
    if (c[2]) { c[1] = 0; c[2] = 0; }
    if (c[1]) c[0] = 0;                                             /* tripped: run command dropped */
    int32_t f = h[0];
    int32_t target = c[0] ? h[2] : 0;
    if (h[5] != p->st[0]) {                                         /* direction change: stop, then reverse */
        if (f > 0) target = 0; else p->st[0] = h[5];
    }
    int32_t rate = 500 / (h[4] > 0 ? h[4] : 1);
    if (rate < 1) rate = 1;
    f = c[1] ? ramp(f, 0, 0, 60) : ramp(f, target, rate, rate);
    h[0] = (int16_t)f;
    h[1] = (int16_t)clamp32((f ? 20 + f * 3 / 10 : 0) + p->offset[1], 0, 32000);
    h[3] = (int16_t)clamp32(30 + f / 25 + p->offset[3], -32000, 32000);
    h[6] = (int16_t)(f * h[1] / 5000);
}

static void step_conveyor(sim_plc_t *p)
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

static void step_boiler(sim_plc_t *p)
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

static void step_pumps(sim_plc_t *p)
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

static void step_compressor(sim_plc_t *p)
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

static void step_ahu(sim_plc_t *p)
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

static void step_vfd(sim_plc_t *p)
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

void sim_step(sim_plc_t *p)
{
    switch (p->type) {
        case SIM_TANK:       step_tank(p); break;
        case SIM_DRIVE:      step_drive(p); break;
        case SIM_CONVEYOR:   step_conveyor(p); break;
        case SIM_BOILER:     step_boiler(p); break;
        case SIM_PUMPS:      step_pumps(p); break;
        case SIM_COMPRESSOR: step_compressor(p); break;
        case SIM_AHU:        step_ahu(p); break;
        case SIM_VFD:        step_vfd(p); break;
        default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* Modbus TCP server                                                          */
/* ------------------------------------------------------------------------- */
static uint16_t be16(const uint8_t *b) { return (uint16_t)((b[0] << 8) | b[1]); }
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }

int32_t sim_frame_length(const uint8_t *buf, size_t len)
{
    if (len < 7) return 0;
    uint16_t proto = be16(buf + 2), plen = be16(buf + 4);
    if (proto != 0 || plen < 2 || plen > SIM_MAX_ADU - 6) return -1;
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

static size_t exception(sim_plc_t *p, uint8_t *out, size_t cap, uint16_t tid, uint8_t unit, uint8_t fc, uint8_t code)
{
    uint8_t pdu[2] = { (uint8_t)(fc | 0x80), code };
    p->exceptions++;
    return reply(out, cap, tid, unit, pdu, 2);
}

/* First coil in mask that is set, for the refusal message */
static const sim_row_t *blocker(const sim_plc_t *p, uint8_t mask)
{
    for (uint8_t b = 0; b < SIM_COILS; b++)
        if ((mask & (1u << b)) && p->coils[b]) return find_row(p, true, b);
    return NULL;
}

size_t sim_handle(sim_plc_t *p, const uint8_t *adu, size_t len, uint8_t *out, size_t cap)
{
    int32_t flen = sim_frame_length(adu, len);
    if (flen <= 0 || (size_t)flen < 8) return 0;
    uint16_t tid = be16(adu);
    uint8_t unit = adu[6];
    const uint8_t *pdu = adu + 7;
    size_t plen = (size_t)flen - 7;
    uint8_t fc = pdu[0];
    const sim_type_info_t *ti = sim_type_info(p->type);

    p->requests++;
    if (p->freeze) return 0;

    if (fc == 0x2B) {                                                     /* FC43 / MEI 0x0E device identification */
        if (plen < 2 || pdu[1] != 0x0E || !ti->vendor) return exception(p, out, cap, tid, unit, fc, 0x01);
        uint8_t r[3 * (2 + 32) + 7];
        size_t n = 0;
        r[n++] = 0x2B; r[n++] = 0x0E; r[n++] = 0x01; r[n++] = 0x01; r[n++] = 0x00; r[n++] = 0x00; r[n++] = 3;
        const char *obj[3] = { ti->vendor, ti->product, ti->revision };
        for (uint8_t i = 0; i < 3; i++) {
            size_t sl = strlen(obj[i]);
            if (sl > 32) sl = 32;
            r[n++] = i; r[n++] = (uint8_t)sl;
            memcpy(r + n, obj[i], sl);
            n += sl;
        }
        return reply(out, cap, tid, unit, r, n);
    }
    if (p->errors) return exception(p, out, cap, tid, unit, fc, 0x04);     /* server device failure */
    if (plen < 5) return exception(p, out, cap, tid, unit, fc, 0x03);

    uint16_t addr = be16(pdu + 1), qv = be16(pdu + 3);
    char msg[SIM_EVENT_LEN], val[32];
    if (fc == 0x03 || fc == 0x01) {
        uint16_t size = fc == 0x03 ? SIM_HR : SIM_COILS;
        if (qv == 0 || qv > 125 || (uint32_t)addr + qv > size) return exception(p, out, cap, tid, unit, fc, 0x02);
        uint8_t r[2 + 2 * SIM_HR];
        size_t n = 0;
        r[n++] = fc;
        if (fc == 0x03) {
            r[n++] = (uint8_t)(qv * 2);
            for (uint16_t i = 0; i < qv; i++) { put16(r + n, (uint16_t)p->hr[addr + i]); n += 2; }
        } else {
            uint8_t bytes = (uint8_t)((qv + 7) / 8);
            r[n++] = bytes;
            memset(r + n, 0, bytes);
            for (uint16_t i = 0; i < qv; i++) if (p->coils[addr + i]) r[n + i / 8] |= (uint8_t)(1u << (i % 8));
            n += bytes;
        }
        return reply(out, cap, tid, unit, r, n);
    }
    if (fc == 0x05) {
        const sim_row_t *row = addr < SIM_COILS ? find_row(p, true, addr) : NULL;
        if (!row || !(row->flags & SIM_HMI_W)) return exception(p, out, cap, tid, unit, fc, 0x02);
        if (qv != 0xFF00 && qv != 0x0000) return exception(p, out, cap, tid, unit, fc, 0x03);
        bool on = qv == 0xFF00;
        const sim_row_t *blk = on ? blocker(p, row->block_mask) : NULL;
        if (blk) {
            snprintf(msg, sizeof(msg), "HMI tried to switch %s ON: refused, %s is active", row->label, blk->label);
            event(p, msg);                                            /* interlock: the PLC keeps it off */
        } else {
            p->coils[addr] = on ? 1 : 0;
            if (row->flags & SIM_PULSE) { if (on) { snprintf(msg, sizeof(msg), "HMI pressed %s", row->label); event(p, msg); } }
            else { snprintf(msg, sizeof(msg), "HMI switched %s %s", row->label, on ? "ON" : "OFF"); event(p, msg); }
        }
        p->writes++;
        return reply(out, cap, tid, unit, pdu, 5);
    }
    if (fc == 0x06) {
        const sim_row_t *row = addr < SIM_HR ? find_row(p, false, addr) : NULL;
        if (!row || !(row->flags & SIM_HMI_W)) return exception(p, out, cap, tid, unit, fc, 0x02);
        int32_t v = (int16_t)qv;
        if (v < row->min || v > row->max) return exception(p, out, cap, tid, unit, fc, 0x03);   /* out of range */
        p->hr[addr] = (int16_t)v;
        format_row(row, (int16_t)v, val, sizeof(val));
        snprintf(msg, sizeof(msg), "HMI set %s to %s", row->label, val);
        event(p, msg);
        p->writes++;
        return reply(out, cap, tid, unit, pdu, 5);
    }
    return exception(p, out, cap, tid, unit, fc, 0x01);
}
