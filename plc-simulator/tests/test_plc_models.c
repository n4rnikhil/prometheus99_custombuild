/**
 * @file test_plc_models.c
 * @brief Tests of the PLC Simulator's device models through Modbus frames (no network, no HMI code)
 *
 * Build & run:  build.bat test   (Linux: make test)
 */

#include <stdio.h>
#include <string.h>
#include "plc_models.h"

static int g_fail, g_pass;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* --- a tiny Modbus TCP client: build a request, decode the reply ------------------------- */
typedef struct {
    bool     exception;
    uint8_t  code;            /* exception code */
    uint16_t regs[125];
    uint8_t  nregs;
    uint8_t  coils[16];
    bool     replied;
} reply_t;

static reply_t g_r;

static void req(sim_plc_t *p, uint8_t fc, uint16_t addr, uint16_t qv)
{
    uint8_t adu[12] = { 0x12, 0x34, 0, 0, 0, 6, 1, fc, (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(qv >> 8), (uint8_t)qv };
    uint8_t out[SIM_MAX_ADU];
    memset(&g_r, 0, sizeof(g_r));
    size_t n = sim_handle(p, adu, sizeof(adu), out, sizeof(out));
    g_r.replied = n > 0;
    if (n < 9) return;
    if (out[7] & 0x80) { g_r.exception = true; g_r.code = out[8]; return; }
    if (fc == 0x03) {
        g_r.nregs = (uint8_t)(out[8] / 2);
        for (uint8_t i = 0; i < g_r.nregs; i++) g_r.regs[i] = (uint16_t)((out[9 + 2 * i] << 8) | out[10 + 2 * i]);
    } else if (fc == 0x01) {
        for (uint8_t i = 0; i < out[8] && i < sizeof(g_r.coils); i++) g_r.coils[i] = out[9 + i];
    }
}

static bool ok(void) { return g_r.replied && !g_r.exception; }
static bool exc(uint8_t code) { return g_r.replied && g_r.exception && g_r.code == code; }
static bool wr_reg(sim_plc_t *p, uint16_t a, int16_t v) { req(p, 0x06, a, (uint16_t)v); return ok(); }
static bool wr_coil(sim_plc_t *p, uint16_t a, bool on) { req(p, 0x05, a, on ? 0xFF00 : 0); return ok(); }

static float f32(uint16_t hi, uint16_t lo)
{
    uint32_t u = ((uint32_t)hi << 16) | lo;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* FC43: objects of the reply as "id=text;" pairs */
static bool devid(sim_plc_t *p, uint8_t code, char *objs, size_t cap)
{
    uint8_t adu[11] = { 0x43, 0x01, 0, 0, 0, 5, 1, 0x2B, 0x0E, code, 0 };
    uint8_t out[SIM_MAX_ADU];
    size_t n = sim_handle(p, adu, sizeof(adu), out, sizeof(out));
    objs[0] = '\0';
    if (n < 14 || out[7] != 0x2B) return false;
    size_t pos = 14, len = 0;
    for (uint8_t i = 0; i < out[13] && pos + 2 <= n; i++) {
        uint8_t id = out[pos], ol = out[pos + 1];
        int k = snprintf(objs + len, cap - len, "%u=%.*s;", (unsigned)id, (int)ol, (const char *)out + pos + 2);
        if (k > 0) len += (size_t)k;
        pos += 2u + ol;
    }
    return true;
}

static void steps(sim_plc_t *p, int n) { for (int i = 0; i < n; i++) sim_step(p); }

static int count_events(sim_plc_t *p, const char *needle)
{
    char ev[SIM_EVENT_LEN];
    int hits = 0;
    while (sim_pop_event(p, ev, sizeof(ev))) if (!needle || strstr(ev, needle)) hits++;
    return hits;
}

/* ------------------------------------------------------------------------- */
static void test_tables(void)
{
    static sim_plc_t p;
    for (int ty = 0; ty < SIM_TYPE_COUNT; ty++) {
        const sim_type_info_t *ti = sim_type_info((sim_type_t)ty);
        CHECK(ti && ti->nrows > 0 && ti->nrows <= SIM_MAX_ROWS && ti->nregs <= SIM_MAX_REGS);
        if (!ti) continue;
        CHECK((ti->vendor != NULL) == (ti->product != NULL) && (ty == SIM_ANON || ti->vendor));
        CHECK(!ti->app || ti->vendor);                                         /* a program name needs an identity */
        for (uint8_t r = 0; r < ti->nrows; r++) {
            const sim_row_t *row = &ti->rows[r];
            CHECK(row->addr < ((row->flags & SIM_COIL) ? SIM_COILS : SIM_HR) && row->min <= row->max);
            if (row->options) {
                int opts = 1;
                for (const char *o = row->options; *o; o++) if (*o == '|') opts++;
                CHECK(row->min == 0 && row->max == opts - 1);
            }
            if (row->flags & SIM_PULSE) CHECK((row->flags & SIM_COIL) && (row->flags & SIM_HMI_W));
            if (!(row->flags & SIM_COIL)) CHECK(sim_reg_address(ti, row->addr) >= 0);   /* every register is reachable */
            for (uint8_t r2 = (uint8_t)(r + 1); r2 < ti->nrows; r2++)       /* no address twice */
                CHECK(!(((ti->rows[r2].flags ^ row->flags) & SIM_COIL) == 0 && ti->rows[r2].addr == row->addr));
        }
        for (uint8_t i = 0; i < ti->nregs; i++)                               /* sparse maps do not overlap */
            for (uint8_t k = (uint8_t)(i + 1); k < ti->nregs; k++) {
                uint32_t a0 = ti->regs[i].addr, a1 = a0 + (ti->regs[i].fmt == SIM_FMT_FLOAT32 ? 1u : 0u);
                uint32_t b0 = ti->regs[k].addr, b1 = b0 + (ti->regs[k].fmt == SIM_FMT_FLOAT32 ? 1u : 0u);
                CHECK(a1 < b0 || b1 < a0);
            }
        /* defaults stay finite and the model never runs away in 5 minutes */
        sim_init(&p, (sim_type_t)ty);
        steps(&p, 3000);
        for (int k = 0; k < SIM_HR; k++) CHECK(p.hr[k] > -32000 && p.hr[k] < 32000);
    }
    CHECK(sim_type_info(SIM_TYPE_COUNT) == NULL);
}

static void test_tank_and_frames(void)
{
    static sim_plc_t p;
    char objs[200];
    sim_init(&p, SIM_TANK);
    req(&p, 0x03, 0, 7);
    CHECK(ok() && g_r.nregs == 7 && g_r.regs[0] == 720 && g_r.regs[2] == 65 && g_r.regs[5] == 600 && g_r.regs[6] == 0);
    req(&p, 0x03, 14, 4);
    CHECK(exc(0x02));                                                         /* past the image */
    req(&p, 0x01, 0, 5);
    CHECK(ok() && (g_r.coils[0] & 1) && !(g_r.coils[0] & 4));
    /* identity: basic objects, and the program name in the regular category */
    CHECK(devid(&p, 1, objs, sizeof(objs)) && strstr(objs, "0=Schneider Electric;") && strstr(objs, "1=BMX P34 2020;"));
    CHECK(strstr(objs, "6=") == NULL);
    CHECK(devid(&p, 2, objs, sizeof(objs)) && strstr(objs, "6=HMI-TANK;"));

    CHECK(wr_reg(&p, 2, 25) && p.hr[2] == 25);
    CHECK(!wr_reg(&p, 2, 250) && exc(0x03) && p.hr[2] == 25);                 /* out of range: refused */
    CHECK(!wr_reg(&p, 0, 5) && exc(0x02));                                    /* level is read-only */
    CHECK(!wr_coil(&p, 4, true));                                             /* overload is a field input */
    CHECK(count_events(&p, "HMI set Valve V-101 to 25 %") == 1);
    CHECK(wr_coil(&p, 2, true));                                              /* stop latch */
    steps(&p, 1);
    CHECK(p.coils[0] == 0 && p.coils[1] == 0);
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);                          /* start refused: latched */
    CHECK(count_events(&p, "refused, Stop latch is active") == 1);
    CHECK(wr_coil(&p, 2, false));
    p.coils[4] = 1;                                                           /* field: pump A overload */
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);
    CHECK(count_events(&p, "Pump A overload is active") == 1);
    CHECK(wr_coil(&p, 3, true));                                              /* reset (pulse) */
    steps(&p, 1);
    CHECK(p.coils[4] == 0 && p.coils[3] == 0);
    CHECK(count_events(&p, "HMI pressed Fault reset") == 1);
    /* Auto: the PLC starts pump A below the setpoint band and stops it above */
    CHECK(wr_reg(&p, 5, 900) && wr_reg(&p, 6, 1));
    steps(&p, 2);
    CHECK(p.coils[0] == 1);
    p.hr[0] = 950;
    steps(&p, 1);
    CHECK(p.coils[0] == 0);
    CHECK(!wr_reg(&p, 6, 2));                                                 /* only Manual / Auto */
    char txt[32];
    sim_row_text(&p, 6, txt, sizeof(txt));
    CHECK(strcmp(txt, "Auto") == 0);
    sim_row_text(&p, 0, txt, sizeof(txt));
    CHECK(strncmp(txt, "95.", 3) == 0 && strstr(txt, " %") != NULL);
    /* RUN / STOP: in STOP the program does not scan, the server still answers */
    sim_set_stopped(&p, true);
    int16_t level = p.hr[0], speed = p.hr[1];
    steps(&p, 50);
    CHECK(p.hr[0] == level && p.hr[1] == speed && count_events(&p, "STOP") == 1);
    req(&p, 0x03, 0, 2);
    CHECK(ok() && g_r.regs[0] == (uint16_t)level);
    sim_set_stopped(&p, false);
    CHECK(count_events(&p, "RUN") == 1);
    /* fault modes and malformed frames */
    p.freeze = true;
    req(&p, 0x03, 0, 1);
    CHECK(!g_r.replied);
    p.freeze = false;
    p.errors = true;
    req(&p, 0x03, 0, 1);
    CHECK(exc(0x04));
    p.errors = false;
    uint8_t junk[12] = { 0, 1, 0x12, 0x34, 0, 6, 1, 3, 0, 0, 0, 1 };
    uint8_t out[SIM_MAX_ADU];
    CHECK(sim_frame_length(junk, sizeof(junk)) == -1 && sim_handle(&p, junk, sizeof(junk), out, sizeof(out)) == 0);
    CHECK(sim_frame_length(junk, 6) == 0);
    /* nudges: stored values move, computed values keep an offset through the process */
    sim_init(&p, SIM_TANK);
    sim_nudge(&p, 0, +1);
    CHECK(p.hr[0] == 770);
    steps(&p, 1);
    int16_t t0 = p.hr[4];
    sim_nudge(&p, 4, +1); sim_nudge(&p, 4, +1);
    steps(&p, 1);
    CHECK(p.hr[4] >= t0 + 9);
    sim_nudge(&p, 1, +1);                                                     /* pump speed: process only */
    steps(&p, 1);
    CHECK(p.hr[1] == 1450);
    /* the event ring keeps the newest when it overflows */
    sim_init(&p, SIM_TANK);
    for (int k = 0; k < SIM_EVENTS + 3; k++) wr_reg(&p, 2, (int16_t)k);
    CHECK(count_events(&p, NULL) == SIM_EVENTS);
}

/* Altivar: real parameter addresses and the CiA402 state machine */
enum { ETA = 3201, RFR = 3202, LCR = 3204, LFT = 7121, CMD = 8501, LFR = 8502, ACC = 9001 };
static uint16_t rd1(sim_plc_t *p, uint16_t a) { req(p, 0x03, a, 1); return ok() ? g_r.regs[0] : 0xDEADu; }
static bool running(sim_plc_t *p) { return (rd1(p, ETA) & 0x006F) == 0x0027; }

static void test_altivar(void)
{
    static sim_plc_t p;
    char objs[200];
    sim_init(&p, SIM_ATV340);
    steps(&p, 40);
    CHECK(running(&p) && (int16_t)rd1(&p, RFR) == 300 && (rd1(&p, ETA) & 0x0200));   /* network has control */
    req(&p, 0x03, 3201, 4);
    CHECK(ok() && g_r.nregs == 4 && g_r.regs[1] == 300 && g_r.regs[3] > 0);
    req(&p, 0x03, 3200, 2);
    CHECK(exc(0x02));                                                         /* not a parameter */
    req(&p, 0x01, 0, 1);
    CHECK(exc(0x01));                                                         /* drives have no coils */
    CHECK(!wr_reg(&p, ETA, 0) && exc(0x02));                                  /* status word is read-only */
    CHECK(!wr_reg(&p, LFR, 700) && exc(0x03));
    CHECK(devid(&p, 2, objs, sizeof(objs)) && strstr(objs, "1=ATV340U07N4E;") && !strstr(objs, "6="));

    /* stop by ramp, then run again at a new speed */
    CHECK(wr_reg(&p, CMD, 0x0007));
    CHECK((rd1(&p, ETA) & 0x006F) == 0x0023 && count_events(&p, "switched on") == 1);
    steps(&p, 60);
    CHECK(rd1(&p, RFR) == 0);
    CHECK(wr_reg(&p, LFR, 450) && wr_reg(&p, CMD, 0x000F));
    steps(&p, 60);
    CHECK(running(&p) && rd1(&p, RFR) == 450 && (rd1(&p, ETA) & 0x0400));     /* reference reached */
    CHECK(wr_reg(&p, ACC, 100) && p.hr[6] == 100);

    /* reverse through the full sequence */
    CHECK(wr_reg(&p, CMD, 0x0006) && wr_reg(&p, CMD, 0x0007) && wr_reg(&p, CMD, 0x080F));
    steps(&p, 200);
    CHECK(running(&p) && (int16_t)rd1(&p, RFR) == -450 && (rd1(&p, ETA) & 0x8000));

    /* overload: trips, refuses a reset while present, resets after; a run needs 6-7-F again */
    p.coils[0] = 1;
    steps(&p, 20);
    CHECK((rd1(&p, ETA) & 0x0008) && rd1(&p, RFR) == 0 && rd1(&p, LFT) == 16 && count_events(&p, "tripped") == 1);
    CHECK(wr_reg(&p, CMD, 0x000F) && !running(&p));
    CHECK(wr_reg(&p, CMD, 0x0080) && (rd1(&p, ETA) & 0x0008) && count_events(&p, "refused") == 1);
    p.coils[0] = 0;
    CHECK(wr_reg(&p, CMD, 0x0000) && wr_reg(&p, CMD, 0x0080) && !(rd1(&p, ETA) & 0x0008));
    CHECK(wr_reg(&p, CMD, 0x000F) && !running(&p));                          /* switch on disabled: 6 first */
    CHECK(wr_reg(&p, CMD, 0x0006) && wr_reg(&p, CMD, 0x0007) && wr_reg(&p, CMD, 0x000F) && running(&p));

    /* local (keypad) control: the network loses control, commands are ignored */
    p.coils[1] = 1;
    steps(&p, 1);
    CHECK(!(rd1(&p, ETA) & 0x0200));
    CHECK(wr_reg(&p, CMD, 0x0007) && running(&p) && count_events(&p, "local") == 1);
    p.coils[1] = 0;

    /* over-current from a heavier load trips the drive */
    for (int k = 0; k < 20; k++) sim_nudge(&p, 3, +1);
    steps(&p, 2);
    CHECK((rd1(&p, ETA) & 0x0008) && count_events(&p, "over-current") == 1);

    /* the ATV630 is the same drive with another identity */
    sim_init(&p, SIM_ATV630);
    CHECK(devid(&p, 1, objs, sizeof(objs)) && strstr(objs, "1=ATV630U07N4;") && running(&p));
}

/* EM6400NG: IEEE floats, two registers each, high word first */
static void test_meter(void)
{
    static sim_plc_t p;
    char objs[200];
    sim_init(&p, SIM_METER);
    steps(&p, 1);
    req(&p, 0x03, 2999, 6);
    CHECK(ok() && g_r.nregs == 6);
    float ia = f32(g_r.regs[0], g_r.regs[1]), ic = f32(g_r.regs[4], g_r.regs[5]);
    CHECK(ia > 5.11f && ia < 5.13f && ic > 5.04f && ic < 5.06f);
    req(&p, 0x03, 3019, 8);
    CHECK(ok() && f32(g_r.regs[6], g_r.regs[7]) > 415.0f && f32(g_r.regs[6], g_r.regs[7]) < 415.4f);
    req(&p, 0x03, 3059, 2);
    float kw = f32(g_r.regs[0], g_r.regs[1]);
    CHECK(ok() && kw > 3.2f && kw < 3.35f);                                   /* sqrt(3) x 415 V x 5 A x 0.9 */
    req(&p, 0x03, 3109, 2);
    CHECK(ok() && f32(g_r.regs[0], g_r.regs[1]) > 49.99f && f32(g_r.regs[0], g_r.regs[1]) < 50.01f);
    req(&p, 0x03, 3000, 2);
    CHECK(ok());                                                              /* a read may start mid-float */
    req(&p, 0x03, 3005, 2);
    CHECK(exc(0x02));                                                         /* 3006 is not in the list */
    req(&p, 0x03, 2699, 2);
    float e0 = f32(g_r.regs[0], g_r.regs[1]);
    steps(&p, 11100);                                                         /* about 18.5 minutes */
    req(&p, 0x03, 2699, 2);
    CHECK(f32(g_r.regs[0], g_r.regs[1]) >= e0 + 1.0f);                        /* 3.27 kW for 18.5 min = 1 kWh */
    CHECK(!wr_reg(&p, 2999, 1) && exc(0x02));                                 /* read only */
    req(&p, 0x05, 0, 0xFF00);
    CHECK(exc(0x01));
    CHECK(devid(&p, 2, objs, sizeof(objs)) && strstr(objs, "1=EM6400NG;") && !strstr(objs, "6="));
    CHECK(!sim_type_info(SIM_METER)->is_plc);
}

static void test_other_plcs(void)
{
    static sim_plc_t p;
    char objs[200];
    /* conveyor: speed, counting, jam, pull-cord, resets */
    sim_init(&p, SIM_CONVEYOR);
    steps(&p, 600);                                                           /* 60 s at 30 m/min */
    CHECK(p.hr[0] == 300 && p.hr[2] >= 29 && p.hr[2] <= 31 && p.hr[4] == 30);
    p.coils[1] = 1;                                                           /* jam */
    steps(&p, 20);
    CHECK(p.hr[0] == 0 && p.coils[0] == 0);
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);
    CHECK(wr_coil(&p, 3, true));
    steps(&p, 1);
    CHECK(p.coils[1] == 0 && wr_coil(&p, 0, true) && p.coils[0] == 1);
    CHECK(wr_coil(&p, 4, true));
    steps(&p, 1);
    CHECK(p.hr[2] == 0);
    CHECK(wr_coil(&p, 2, true));                                              /* pull-cord */
    steps(&p, 1);
    CHECK(p.coils[0] == 0);
    CHECK(devid(&p, 2, objs, sizeof(objs)) && strstr(objs, "6=HMI-CONVEYOR;"));

    /* boiler: auto control holds the setpoint; low water trips and latches */
    sim_init(&p, SIM_BOILER);
    steps(&p, 3000);
    CHECK(p.hr[0] >= 780 && p.hr[0] <= 820);
    CHECK(wr_reg(&p, 5, 0) && wr_reg(&p, 6, 0));                             /* manual 0 % */
    steps(&p, 5);
    CHECK(p.hr[2] == 0 && p.coils[1] == 0);
    p.hr[4] = 10;                                                             /* drum runs dry */
    steps(&p, 1);
    CHECK(p.coils[2] == 1 && p.coils[0] == 0);
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);
    CHECK(wr_coil(&p, 4, true));
    steps(&p, 1);
    CHECK(p.coils[2] == 1);                                                   /* reset refused: still dry */
    p.hr[4] = 50;
    CHECK(wr_coil(&p, 4, true));
    steps(&p, 1);
    CHECK(p.coils[2] == 0);

    /* pump station: auto pressure, standby takeover, hand and off */
    sim_init(&p, SIM_PUMPS);
    steps(&p, 1200);
    CHECK(p.hr[0] >= 360 && p.hr[0] <= 440 && p.coils[0] == 1 && p.coils[1] == 0);
    p.coils[2] = 1;                                                           /* pump 1 fails */
    steps(&p, 600);
    CHECK(p.coils[0] == 0 && p.coils[1] == 1 && p.hr[0] >= 340 && p.hr[0] <= 460);
    CHECK(wr_coil(&p, 4, true));
    steps(&p, 1);
    CHECK(p.coils[2] == 0);
    CHECK(wr_reg(&p, 5, 1) && wr_reg(&p, 6, 40));                            /* hand 40 % */
    steps(&p, 100);
    CHECK(p.hr[3] == 40);
    CHECK(wr_reg(&p, 5, 0));                                                  /* off */
    steps(&p, 50);
    CHECK(p.hr[3] == 0 && p.hr[4] == 0 && p.coils[0] == 0 && p.coils[1] == 0);

    /* compressor: load / unload band, high-temperature trip */
    sim_init(&p, SIM_COMPRESSOR);
    steps(&p, 4000);
    CHECK(p.hr[0] >= 620 && p.hr[0] <= 780 && p.coils[0] == 1);
    for (int k = 0; k < 10; k++) sim_nudge(&p, 3, +1);                        /* +50 C on the discharge */
    steps(&p, 1);
    CHECK(p.coils[1] == 1 && p.coils[0] == 0 && p.hr[5] == 0);
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);

    /* air handler: supply follows the setpoint; fire alarm stops the fan */
    sim_init(&p, SIM_AHU);
    CHECK(wr_reg(&p, 1, 180));
    steps(&p, 200);
    CHECK(p.hr[0] >= 175 && p.hr[0] <= 185 && p.hr[3] == 70);
    p.coils[2] = 1;
    steps(&p, 40);
    CHECK(p.coils[0] == 0 && p.hr[3] == 0);
    CHECK(wr_coil(&p, 0, true) && p.coils[0] == 0);

    /* ABB drive (demo map): speed reference, fault code */
    sim_init(&p, SIM_VFD);
    CHECK(wr_reg(&p, 1, 900));
    steps(&p, 20);
    CHECK(p.hr[0] == 900 && p.hr[3] > 0);
    p.coils[1] = 1;
    steps(&p, 10);
    CHECK(p.hr[0] == 0 && p.hr[5] == 2310);
    CHECK(wr_coil(&p, 2, true));
    steps(&p, 1);
    CHECK(p.hr[5] == 0);

    /* unknown device: no FC43, plain registers */
    sim_init(&p, SIM_ANON);
    CHECK(!devid(&p, 1, objs, sizeof(objs)));
    req(&p, 0x03, 0, 4);
    CHECK(ok() && g_r.regs[3] == 44);
}

int main(void)
{
    test_tables();
    test_tank_and_frames();
    test_altivar();
    test_meter();
    test_other_plcs();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
