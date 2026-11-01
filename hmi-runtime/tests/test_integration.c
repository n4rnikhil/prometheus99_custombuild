/**
 * @file test_integration.c
 * @brief The HMI runtime against the PLC Simulator's device models, in one process
 *
 * The runtime's device manager talks Modbus frames to the simulator's models through its
 * loopback transport (the same state machine as TCP). This checks, for every device type:
 *   - the driver module and the simulated device agree on every address, range, option and interlock
 *   - the module is picked from the device's FC43 identity (and program name for PLC programs)
 *   - operator controls reach the device and change the process, interlocks hold
 *   - real-product maps decode correctly: Altivar status/control words, EM6400NG float registers
 *
 * Needs plc-simulator/src/plc_models.c (the only file shared between the two apps, tests only).
 * Build & run:  build.bat test
 */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "core/tags.h"
#include "core/alarms.h"
#include "core/security.h"
#include "devices/device_profile.h"
#include "fieldbus/device_info.h"
#include "fieldbus/device_manager.h"
#include "fieldbus/discovery.h"
#include "fieldbus/modbus_proto.h"
#include "hal/timer.h"
#include "plc_models.h"

static int g_fail, g_pass;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ------------------------------------------------------------------------- */
/* Module <-> device agreement                                                */
/* ------------------------------------------------------------------------- */
static const sim_row_t *reg_row(const sim_type_info_t *ti, uint32_t addr)
{
    for (uint8_t r = 0; r < ti->nrows; r++) {
        const sim_row_t *row = &ti->rows[r];
        if (!(row->flags & SIM_COIL) && sim_reg_address(ti, row->addr) == (int32_t)addr) return row;
    }
    return NULL;
}

static const sim_row_t *coil_row(const sim_type_info_t *ti, uint32_t addr)
{
    for (uint8_t r = 0; r < ti->nrows; r++)
        if ((ti->rows[r].flags & SIM_COIL) && ti->rows[r].addr == addr) return &ti->rows[r];
    return NULL;
}

static uint8_t wire_format(const sim_type_info_t *ti, uint32_t addr)
{
    for (uint8_t i = 0; i < ti->nregs; i++) if (ti->regs[i].addr == addr) return ti->regs[i].fmt;
    return SIM_FMT_INT16;
}

static void check_block(const hmi_block_map_t *b, bool coil, const sim_type_info_t *ti)
{
    uint8_t step = (!coil && b->format == HMI_FMT_FLOAT32) ? 2 : 1;
    for (uint8_t i = 0; i * step < b->count; i++) {
        if (b->rel_tags[i] == HMI_REL_NONE) continue;
        uint32_t a = b->start + (uint32_t)i * step;
        if (coil) { CHECK(coil_row(ti, a) != NULL); continue; }
        bool mapped = false;                                             /* some image register answers there */
        for (uint8_t k = 0; k < SIM_HR; k++) if (sim_reg_address(ti, k) == (int32_t)a) mapped = true;
        CHECK(mapped);
        CHECK(wire_format(ti, a) == (step == 2 ? SIM_FMT_FLOAT32 : SIM_FMT_INT16));   /* same encoding on the wire */
    }
}

static void check_module_vs_sim(const hmi_device_profile_t *p, sim_type_t st)
{
    const sim_type_info_t *ti = sim_type_info(st);
    check_block(&p->regs, false, ti);
    check_block(&p->coils, true, ti);
    for (uint8_t k = 0; k < p->n_more_regs; k++) check_block(&p->more_regs[k], false, ti);
    for (uint8_t a = 0; a < p->n_actions; a++) {
        const hmi_device_action_t *ac = &p->actions[a];
        if (hmi_act_writes_coil(ac->kind)) {
            const sim_row_t *row = coil_row(ti, ac->address);
            CHECK(row != NULL);
            if (!row) { printf("  %s action %u: no coil %u\n", p->key, a, ac->address); continue; }
            CHECK(row->flags & SIM_HMI_W);                               /* the device accepts this write */
            if (ac->kind == HMI_ACT_PULSE_COIL) CHECK(row->flags & SIM_PULSE);
            if (ac->kind == HMI_ACT_COIL_ON && ac->block_tag != HMI_REL_NONE) {
                int baddr = -1;                                          /* coil that feeds the interlock tag */
                for (uint8_t i = 0; i < p->coils.count; i++) if (p->coils.rel_tags[i] == ac->block_tag) baddr = i;
                CHECK(baddr >= 0 && (row->block_mask & (1u << baddr)));  /* the PLC enforces the same interlock */
            }
            continue;
        }
        const sim_row_t *row = reg_row(ti, ac->address);
        CHECK(row != NULL);
        if (!row) { printf("  %s action %u: no register %u\n", p->key, a, ac->address); continue; }
        CHECK(row->flags & SIM_HMI_W);
        if (ac->kind == HMI_ACT_SET_REG) CHECK(ac->min >= row->min && ac->max <= row->max && ac->min < ac->max);
        if (ac->kind == HMI_ACT_SET_ENUM) CHECK(hmi_action_option_count(ac) == row->max + 1);
        if (ac->kind == HMI_ACT_REG_ON || ac->kind == HMI_ACT_REG_OFF || ac->kind == HMI_ACT_PULSE_REG) {
            CHECK(ac->seq && ac->seq_len >= 1 && ac->seq_len <= HMI_MAX_SEQ);
            if (ac->kind == HMI_ACT_PULSE_REG) CHECK(ac->seq_len >= 2);
            for (uint8_t i = 0; ac->seq && i < ac->seq_len; i++) CHECK(ac->seq[i] >= row->min && ac->seq[i] <= row->max);
        }
    }
    if (p->estop_coil != 0xFFFFu) {
        const sim_row_t *row = coil_row(ti, p->estop_coil);
        CHECK(row && (row->flags & SIM_HMI_W));
    }
}

static void test_modules_match_devices(void)
{
    static const struct { sim_type_t t; const hmi_device_profile_t *p; } pairs[] = {
        { SIM_TANK, &g_profile_tank_skid }, { SIM_ATV340, &g_profile_altivar }, { SIM_ATV630, &g_profile_altivar },
        { SIM_CONVEYOR, &g_profile_conveyor }, { SIM_BOILER, &g_profile_boiler }, { SIM_PUMPS, &g_profile_pump_station },
        { SIM_COMPRESSOR, &g_profile_compressor }, { SIM_AHU, &g_profile_ahu }, { SIM_VFD, &g_profile_abb_drive } };
    for (unsigned k = 0; k < sizeof(pairs) / sizeof(pairs[0]); k++) {
        const sim_type_info_t *ti = sim_type_info(pairs[k].t);
        CHECK(ti->is_plc && hmi_profile_match(ti->vendor, ti->product, ti->app) == pairs[k].p);
        check_module_vs_sim(pairs[k].p, pairs[k].t);
        /* PLC programs need their program name; a drive does not have one */
        if (pairs[k].p->match_app) {
            CHECK(ti->app && hmi_profile_match(ti->vendor, ti->product, NULL) == &g_profile_generic);
            CHECK(hmi_profile_match(ti->vendor, ti->product, "SOMEONE-ELSES-PROGRAM") == &g_profile_generic);
        } else {
            CHECK(!ti->app);
        }
    }
    const sim_type_info_t *m = sim_type_info(SIM_METER);
    CHECK(!m->is_plc && hmi_profile_match(m->vendor, m->product, m->app) == &g_profile_em6400 && !g_profile_em6400.auto_attach);
    check_module_vs_sim(&g_profile_em6400, SIM_METER);
    const sim_type_info_t *an = sim_type_info(SIM_ANON);
    CHECK(hmi_profile_match(an->vendor, an->product, an->app) == &g_profile_generic);
    /* the generic module's raw writes land on writable registers of the unknown device */
    for (uint8_t a = 0; a < g_profile_generic.n_actions; a++) CHECK(a < SIM_HR);
    CHECK(hmi_profile_by_key("altivar") == &g_profile_altivar && hmi_profile_by_key("ALTIVAR") == &g_profile_altivar);
    CHECK(hmi_profile_by_key("nope") == NULL && hmi_profile_by_key("") == NULL && hmi_profile_by_key(NULL) == NULL);
    for (uint8_t i = 0; i < hmi_profile_count(); i++) CHECK(hmi_profile_by_key(hmi_profile_at(i)->key) == hmi_profile_at(i));
}

/* ------------------------------------------------------------------------- */
/* Runtime driving the models                                                  */
/* ------------------------------------------------------------------------- */
static sim_plc_t g_sim[HMI_MAX_DEVICES];
static int g_nsim;
static hmi_tag_table_t g_t;
static hmi_alarm_supervisor_t g_al;

static int8_t g_last_coil[64];          /* last value the HMI wrote to each coil (FC05), -1 = none */

static size_t sim_server(void *ctx, const uint8_t *req, size_t len, uint8_t *rsp, size_t cap)
{
    if (len >= 12 && req[7] == 0x05) {
        uint16_t a = (uint16_t)((req[8] << 8) | req[9]);
        if (a < sizeof(g_last_coil)) g_last_coil[a] = req[10] ? 1 : 0;
    }
    return sim_handle((sim_plc_t *)ctx, req, len, rsp, cap);
}

static void world(void)
{
    hmi_tag_registry_reset();
    hmi_tags_init(&g_t);
    hmi_alarms_init(&g_al);
    hmi_devmgr_init(1000);
    g_nsim = 0;
}

/* Put a simulated device behind the next free slot. key = force a module (like --plc ip@key). */
static bool attach(sim_type_t type, const char *key, uint32_t now)
{
    if (g_nsim >= HMI_MAX_DEVICES) return false;
    const sim_type_info_t *ti = sim_type_info(type);
    sim_init(&g_sim[g_nsim], type);
    if (!hmi_devmgr_attach_loopback(ti->vendor, ti->product, ti->app, key, sim_server, &g_sim[g_nsim], &g_t, &g_al, now))
        return false;
    g_nsim++;
    return true;
}

static void run(uint32_t from, uint32_t to)
{
    for (uint32_t ms = from; ms < to; ms++) {
        if (ms % 100u == 0) for (int i = 0; i < g_nsim; i++) sim_step(&g_sim[i]);     /* one PLC scan per 100 ms */
        hmi_devmgr_poll(&g_t, &g_al, ms);
    }
}

static bool journal_has(const char *text)
{
    for (uint8_t i = 0; i < g_al.journal_count; i++) if (strstr(g_al.journal[i].message, text)) return true;
    return false;
}

static int16_t tag(uint8_t slot, uint8_t rel) { return g_t.tags[HMI_DEV_TAG(slot, rel)].value; }

static void test_tank_scenario(void)
{
    world();
    CHECK(!attach(SIM_METER, NULL, 100));                  /* not a PLC: refused unless the module is forced */
    CHECK(attach(SIM_TANK, NULL, 100));
#if HMI_MAX_DEVICES < 2
    CHECK(!attach(SIM_TANK, NULL, 100));                   /* the only slot is taken */
    hmi_devmgr_shutdown();
    return;
#else
    CHECK(attach(SIM_TANK, NULL, 100));
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    run(100, 600);
    hmi_devmgr_publish(d, 600);
    CHECK(d[0].state == HMI_DEV_ONLINE && d[1].state == HMI_DEV_ONLINE && d[0].profile == &g_profile_tank_skid);
    CHECK(d[0].transport == HMI_TRANSPORT_LOOPBACK && strcmp(d[0].endpoint, "Loopback") == 0);
    CHECK(strcmp(d[0].label, "Tank skid 1") == 0 && strcmp(d[1].label, "Tank skid 2") == 0);   /* unique labels */
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(1, 0));
    CHECK(m && m->group && strcmp(m->group, "Tank skid 2") == 0 && m->tag_id == HMI_DEV_TAG(1, 0));
    enum { LEVEL = 0, VALVE = 2, LEVEL_SP = 5, MODE = 6, PUMP_A = 7, ESTOP = 9 };
    enum { A_START = 0, A_STOP = 1, A_VALVE = 4, A_LEVEL_SP = 5, A_MODE = 6, A_RESET = 7 };
    CHECK(g_t.tags[HMI_DEV_TAG(1, LEVEL)].quality == QUALITY_GOOD && tag(1, PUMP_A) == 1);

    /* Stop on slot 1 only */
    hmi_devmgr_action(1, A_STOP, 0, &g_t, &g_al, 700);
    run(700, 900);
    CHECK(tag(1, PUMP_A) == 0 && tag(0, PUMP_A) == 1);
    CHECK(journal_has("Pump A - Stop pump"));
    hmi_devmgr_action(1, A_START, 0, &g_t, &g_al, 900);
    run(900, 1000);
    CHECK(tag(1, PUMP_A) == 1);
    /* setpoints: in range is written, out of range is refused */
    hmi_devmgr_action(0, A_VALVE, 30, &g_t, &g_al, 1000);
    run(1000, 1100);
    CHECK(tag(0, VALVE) == 30);
    hmi_devmgr_action(0, A_VALVE, 150, &g_t, &g_al, 1100);
    run(1100, 1200);
    CHECK(tag(0, VALVE) == 30 && journal_has("out of range"));
    hmi_devmgr_action(0, A_LEVEL_SP, 800, &g_t, &g_al, 1200);
    /* mode selector: option index is written; invalid option ignored */
    hmi_devmgr_action(0, A_MODE, 1, &g_t, &g_al, 1200);
    hmi_devmgr_action(0, A_MODE, 5, &g_t, &g_al, 1200);
    run(1200, 1300);
    CHECK(tag(0, MODE) == 1 && tag(0, LEVEL_SP) == 800);
    CHECK(journal_has("Control mode -> Auto"));
    /* pulse: logged, and the PLC acts on it */
    hmi_devmgr_action(0, A_RESET, 0, &g_t, &g_al, 1300);
    run(1300, 1900);
    CHECK(journal_has("Pump A overload - Reset"));
    hmi_devmgr_action(0, A_MODE, 0, &g_t, &g_al, 1900);                  /* back to Manual */
    run(1900, 2000);

    /* STOP ALL sets the stop latch of every device that has one, and blocks restart */
    CHECK(hmi_devmgr_estop(true, &g_t, &g_al, 2000) == 2);
    run(2000, 2200);
    CHECK(tag(0, ESTOP) == 1 && tag(0, PUMP_A) == 0);
    hmi_devmgr_action(0, A_START, 0, &g_t, &g_al, 2200);               /* start pump -> blocked by the HMI */
    run(2200, 2400);
    CHECK(tag(0, PUMP_A) == 0 && journal_has("blocked by Stop latch"));
    /* reset refused while the stop-latch alarm is unacknowledged, accepted after ack */
    hmi_alarms_evaluate(&g_al, &g_t, 2400);
    CHECK(hmi_devmgr_estop(false, &g_t, &g_al, 2400) == 0);
    hmi_alarms_acknowledge_all(&g_al, 2401);
    CHECK(hmi_devmgr_estop(false, &g_t, &g_al, 2402) == 2);
    run(2402, 2600);
    CHECK(tag(0, ESTOP) == 0);

    /* a device that is not a known PLC is refused unless explicitly configured */
    hmi_device_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "127.0.0.1");
    c.port = 1599; c.unit = 1; c.identified = true;
    snprintf(c.id.vendor, sizeof(c.id.vendor), "Schneider Electric");
    snprintf(c.id.product, sizeof(c.id.product), "PM5560");
    CHECK(!hmi_devmgr_attach(&c, &g_t, &g_al, 2600));
    c.explicit_target = true;
#if HMI_MAX_DEVICES >= 3
    CHECK(hmi_devmgr_attach(&c, &g_t, &g_al, 2600));
    CHECK(!hmi_devmgr_attach(&c, &g_t, &g_al, 2601));                    /* duplicate */
    hmi_devmgr_publish(d, 2601);
    CHECK(d[2].state == HMI_DEV_CONNECTING && d[2].profile == &g_profile_generic);
#else
    CHECK(!hmi_devmgr_attach(&c, &g_t, &g_al, 2600));                    /* every slot is in use */
#endif
    /* @module forces a driver, an unknown key is not accepted by discovery specs */
    c.port = 1600; snprintf(c.driver, sizeof(c.driver), "em6400");
    CHECK(hmi_devmgr_module_for(&c) == &g_profile_em6400);
    snprintf(c.driver, sizeof(c.driver), "nope");
    CHECK(hmi_devmgr_module_for(&c) == &g_profile_generic);              /* falls back to the identity: PM5560 */
    hmi_devmgr_shutdown();
#endif
}

/* HMI heartbeat: the PLC notices a dead HMI by itself and stops, and nothing restarts on its own */
static void test_hmi_heartbeat(void)
{
    world();
    CHECK(attach(SIM_TANK, NULL, 100));
    run(100, 3000);
    sim_plc_t *t = &g_sim[0];
    int16_t h1 = t->hr[7];
    CHECK(h1 != 0);                                          /* the heartbeat reaches %MW7 */
    run(3000, 6000);
    CHECK(t->hr[7] != h1);                                   /* and keeps changing */
    CHECK(t->coils[2] == 0 && t->coils[0] == 1);             /* no false trip while the HMI is alive */
    /* the HMI dies (or the cable is cut): the PLC keeps scanning, nobody writes %MW7 */
    for (uint32_t ms = 6000; ms < 12000; ms += 100) sim_step(t);
    CHECK(t->coils[2] == 1);                                 /* stop latch set by the PLC itself */
    CHECK(t->coils[0] == 0 && t->coils[1] == 0);             /* pumps off */
    /* the HMI comes back: the latch stays until an operator resets it deliberately */
    run(12000, 15000);
    CHECK(t->coils[2] == 1 && t->coils[0] == 0);
    CHECK(tag(0, g_profile_tank_skid.estop_tag) == 1);       /* and the HMI reads why (Stop latch tag) */
    hmi_devmgr_shutdown();
}

/* STOP ALL must stop EVERY device that can move - not only the ones with a stop latch - and a
 * start queued a moment before must not run after the stop. Checked through the device's own
 * registers, read back over Modbus, for every simulated machine type. */
static const hmi_device_profile_t *prof_of(uint8_t slot)
{
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    hmi_devmgr_publish(d, 0);
    return slot < HMI_MAX_DEVICES ? d[slot].profile : NULL;
}

static bool is_stopped(uint8_t slot)
{
    const hmi_device_profile_t *p = prof_of(slot);
    if (!p) return false;
    if (p->estop_coil != 0xFFFFu && tag(slot, p->estop_tag) == 0) return false;   /* latch must be set */
    bool any = false;
    for (uint8_t k = 0; k < p->n_actions; k++) {
        const hmi_device_action_t *a = &p->actions[k];
        if (hmi_act_is_stop(a->kind)) { any = true; if (tag(slot, a->rel_tag) != 0) return false; }
        else if (a->kind == HMI_ACT_SET_ENUM && a->stop_all) { any = true; if (tag(slot, a->rel_tag) != 0) return false; }
    }
    return any || p->estop_coil != 0xFFFFu;
}

static void stop_all_group(const sim_type_t *types, int n, const char *name)
{
    world();
    for (int i = 0; i < n; i++) CHECK(attach(types[i], NULL, 100));
    run(100, 3000);                                          /* online, running in their start state */
    for (int i = 0; i < n; i++) {                            /* start whatever can be started */
        const hmi_device_profile_t *p = prof_of((uint8_t)i);
        for (uint8_t k = 0; p && k < p->n_actions; k++)
            if (hmi_act_is_start(p->actions[k].kind)) hmi_devmgr_action((uint8_t)i, k, 0, &g_t, &g_al, 3000);
    }
    run(3000, 6000);
    for (int i = 0; i < n; i++) {                            /* a start queued just before STOP ALL ... */
        const hmi_device_profile_t *p = prof_of((uint8_t)i);
        for (uint8_t k = 0; p && k < p->n_actions; k++)
            if (hmi_act_is_start(p->actions[k].kind)) hmi_devmgr_action((uint8_t)i, k, 0, &g_t, &g_al, 6000);
    }
    uint8_t missed = 9;
    uint8_t stopped = hmi_devmgr_stop_all(&g_t, &g_al, 6000, &missed);
    CHECK(stopped == n && missed == 0);
    run(6000, 12000);                                        /* ... never runs after it */
    for (int i = 0; i < n; i++) {
        bool ok = is_stopped((uint8_t)i);
        if (!ok) printf("  STOP ALL did not stop %s device %d (%s)\n", name, i, sim_type_info(types[i])->type_name);
        CHECK(ok);
    }
    hmi_devmgr_shutdown();
}

static void test_stop_all_every_machine(void)
{
    static const sim_type_t a[] = { SIM_ATV340, SIM_TANK, SIM_CONVEYOR, SIM_COMPRESSOR };
    static const sim_type_t b[] = { SIM_ATV630, SIM_BOILER, SIM_PUMPS, SIM_AHU };
#if HMI_MAX_DEVICES >= 4
    stop_all_group(a, 4, "A");
    stop_all_group(b, 4, "B");
    static const sim_type_t c[] = { SIM_VFD };
    stop_all_group(c, 1, "C");
#else
    stop_all_group(a, HMI_MAX_DEVICES, "A");
    stop_all_group(b, HMI_MAX_DEVICES, "B");
#endif
    /* a meter has nothing to stop: it is neither stopped nor missed */
    world();
    CHECK(attach(SIM_METER, "em6400", 100));
    run(100, 2000);
    uint8_t missed = 9;
    CHECK(hmi_devmgr_stop_all(&g_t, &g_al, 2000, &missed) == 0 && missed == 0);
    hmi_devmgr_shutdown();
    /* an offline device is reported, never silently skipped */
    world();
    CHECK(attach(SIM_ATV340, NULL, 100));
    run(100, 2000);
    g_sim[0].freeze = true;                                  /* link dead: no replies */
    uint32_t t = 2000;
    hmi_device_info_t d[HMI_MAX_DEVICES];
    do { run(t, t + 10); t += 10; hmi_devmgr_publish(d, t); } while (d[0].state != HMI_DEV_OFFLINE && t < 12000);
    CHECK(d[0].state == HMI_DEV_OFFLINE);
    missed = 0;
    CHECK(hmi_devmgr_stop_all(&g_t, &g_al, t, &missed) == 0 && missed == 1);
    CHECK(journal_has("OFFLINE, not stopped"));
    hmi_devmgr_shutdown();
}

/* More devices than the 4 a small screen shows: all of them attach, and STOP ALL stops every one */
static void test_eight_devices(void)
{
#if HMI_MAX_DEVICES >= 8
    static const sim_type_t all[] = { SIM_ATV340, SIM_TANK, SIM_CONVEYOR, SIM_COMPRESSOR,
                                      SIM_ATV630, SIM_BOILER, SIM_PUMPS, SIM_AHU };
    stop_all_group(all, 8, "eight");
#endif
}

static hmi_device_candidate_t candidate(const char *host, uint16_t port, sim_type_t type, bool explicit_target)
{
    hmi_device_candidate_t c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "%s", host);
    c.port = port;
    c.unit = 1;
    c.identified = true;
    c.explicit_target = explicit_target;
    const sim_type_info_t *ti = sim_type_info(type);
    snprintf(c.id.vendor, sizeof(c.id.vendor), "%s", ti->vendor);
    snprintf(c.id.product, sizeof(c.id.product), "%s", ti->product);
    snprintf(c.id.app, sizeof(c.id.app), "%s", ti->app ? ti->app : "");
    return c;
}

/* Every place in use: a further PLC is named, raises "Devices not shown", and STOP ALL counts it as
 * NOT stopped instead of reporting success. A --plc device gets a place ahead of auto-found ones. */
static void test_device_overflow(void)
{
    world();
    for (int i = 0; i < HMI_MAX_DEVICES; i++) CHECK(attach(SIM_TANK, NULL, 100));
    run(100, 2000);
    hmi_device_candidate_t extra = candidate("10.9.9.9", 502, SIM_TANK, false);
    CHECK(!hmi_devmgr_attach(&extra, &g_t, &g_al, 2000));
    CHECK(journal_has("10.9.9.9:502 NOT shown"));
    CHECK(hmi_devmgr_unattached(2000) == 1);
    uint8_t missed = 0;
    CHECK(hmi_devmgr_stop_all(&g_t, &g_al, 2000, &missed) == HMI_MAX_DEVICES && missed == 1);
    CHECK(journal_has("10.9.9.9:502 not on this HMI"));
    CHECK(hmi_devmgr_unattached(2000 + 20000) == 0);        /* gone from the network: forgotten */
    hmi_devmgr_shutdown();

#if HMI_MAX_DEVICES >= 2
    world();
    hmi_discovery_clear();
    CHECK(hmi_discovery_add("10.9.9.10:502", true));          /* --plc 10.9.9.10 */
    for (int i = 0; i < HMI_MAX_DEVICES - 1; i++) CHECK(attach(SIM_TANK, NULL, 100));
    hmi_device_candidate_t auto_c = candidate("10.9.9.11", 502, SIM_TANK, false);
    CHECK(!hmi_devmgr_attach(&auto_c, &g_t, &g_al, 200));    /* the last place is kept ... */
    CHECK(journal_has("place kept for a --plc device"));
    hmi_device_candidate_t mine = candidate("10.9.9.10", 502, SIM_METER, true);
    CHECK(hmi_devmgr_attach(&mine, &g_t, &g_al, 200));       /* ... for the configured device */
    hmi_discovery_clear();
    hmi_devmgr_shutdown();
#endif
}

/* Two reset buttons pressed within the pulse time: both inputs are released (never left on) */
static void test_reset_pulses(void)
{
    world();
    memset(g_last_coil, -1, sizeof(g_last_coil));
    CHECK(attach(SIM_CONVEYOR, NULL, 100));
    run(100, 2000);
    const hmi_device_profile_t *p = prof_of(0);
    uint8_t jam = 0xFF, count = 0xFF;
    for (uint8_t k = 0; p && k < p->n_actions; k++) {
        if (p->actions[k].kind != HMI_ACT_PULSE_COIL) continue;
        if (jam == 0xFF) jam = k; else count = k;
    }
    CHECK(jam != 0xFF && count != 0xFF);
    hmi_devmgr_action(0, jam, 0, &g_t, &g_al, 2000);
    hmi_devmgr_action(0, count, 0, &g_t, &g_al, 2001);         /* second reset while the first is held */
    run(2000, 4000);
    CHECK(g_last_coil[p->actions[jam].address] == 0 && g_last_coil[p->actions[count].address] == 0);
    hmi_devmgr_shutdown();
}

static void test_every_plc_comes_online(void)
{
    static const sim_type_t types[] = { SIM_ATV340, SIM_ATV630, SIM_CONVEYOR, SIM_BOILER, SIM_PUMPS, SIM_COMPRESSOR, SIM_AHU, SIM_VFD };
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    for (unsigned k = 0; k < sizeof(types) / sizeof(types[0]); k++) {
        world();
        CHECK(attach(types[k], NULL, 100));
        run(100, 500);
        hmi_devmgr_publish(d, 500);
        const sim_type_info_t *ti = sim_type_info(types[k]);
        CHECK(d[0].state == HMI_DEV_ONLINE && d[0].profile == hmi_profile_match(ti->vendor, ti->product, ti->app));
        CHECK(tag(0, d[0].profile->comm_tag) == 0);
        CHECK(strcmp(d[0].product, ti->product) == 0);
        hmi_devmgr_shutdown();
    }
}

/* Altivar: status word -> run / fault / remote bits, control-word sequences, ramps and a fault reset */
static void test_altivar_control(void)
{
    enum { FREQ = 1, FREQ_REF = 2, CURRENT = 3, LAST_FAULT = 4, SPEED_SP = 6, ACCEL = 7, RUN = 9, FAULT = 10, REMOTE = 11, REVERSE = 12 };
    enum { A_RUN_FWD = 0, A_RUN_REV = 1, A_STOP = 2, A_SET_SPEED = 3, A_SET_ACC = 4, A_SET_DEC = 5, A_RESET = 6 };
    world();
    CHECK(attach(SIM_ATV340, NULL, 100));
    run(100, 700);
    CHECK(tag(0, RUN) == 1 && tag(0, FAULT) == 0 && tag(0, REMOTE) == 1 && tag(0, REVERSE) == 0);
    CHECK(tag(0, FREQ) == 300 && tag(0, FREQ_REF) == 300 && tag(0, CURRENT) > 0 && tag(0, SPEED_SP) == 300);
    CHECK(g_profile_altivar.n_bits == 4 && g_profile_altivar.n_more_regs == 3);

    /* 1 s ramps (0 -> 50 Hz), so the test does not wait for the 5 s factory ramps */
    hmi_devmgr_action(0, A_SET_ACC, 10, &g_t, &g_al, 700);
    hmi_devmgr_action(0, A_SET_DEC, 10, &g_t, &g_al, 700);
    run(700, 900);
    CHECK(tag(0, ACCEL) == 10 && tag(0, 8) == 10);

    hmi_devmgr_action(0, A_STOP, 0, &g_t, &g_al, 900);                 /* CMD = 0x0007: ramp stop */
    run(900, 2000);
    CHECK(tag(0, RUN) == 0 && tag(0, FREQ) == 0 && journal_has("Drive - Stop"));
    hmi_devmgr_action(0, A_SET_SPEED, 450, &g_t, &g_al, 2000);         /* LFR = 45.0 Hz */
    hmi_devmgr_action(0, A_RUN_FWD, 0, &g_t, &g_al, 2000);             /* 0x0006, 0x0007, 0x000F */
    run(2000, 3300);
    CHECK(tag(0, RUN) == 1 && tag(0, FREQ) == 450 && tag(0, SPEED_SP) == 450);
    hmi_devmgr_action(0, A_SET_ACC, 20, &g_t, &g_al, 3300);            /* tuning command: 2 s ramp */
    run(3300, 3500);
    CHECK(tag(0, ACCEL) == 20);

    hmi_devmgr_action(0, A_RUN_REV, 0, &g_t, &g_al, 3500);             /* bit 11 = reverse: decelerate, then reverse */
    run(3500, 9000);
    CHECK(tag(0, RUN) == 1 && tag(0, REVERSE) == 1 && tag(0, FREQ) == -450);

    /* an overload trips the drive: fault bit, last fault code, run is blocked, reset needs the cause gone */
    g_sim[0].coils[0] = 1;
    run(9000, 9600);
    CHECK(tag(0, FAULT) == 1 && tag(0, RUN) == 0 && tag(0, LAST_FAULT) == 16);
    hmi_devmgr_action(0, A_RUN_FWD, 0, &g_t, &g_al, 9600);             /* blocked by the fault interlock */
    run(9600, 9800);
    CHECK(tag(0, RUN) == 0 && journal_has("blocked by Drive fault"));
    hmi_devmgr_action(0, A_RESET, 0, &g_t, &g_al, 9800);               /* 0x0080 then 0x0000 */
    run(9800, 10800);
    CHECK(tag(0, FAULT) == 1);                                           /* still overloaded */
    g_sim[0].coils[0] = 0;
    hmi_devmgr_action(0, A_RESET, 0, &g_t, &g_al, 10800);
    run(10800, 11800);
    CHECK(tag(0, FAULT) == 0);
    hmi_devmgr_action(0, A_RUN_FWD, 0, &g_t, &g_al, 11800);
    run(11800, 14500);
    CHECK(tag(0, RUN) == 1 && tag(0, FREQ) == 450);

    /* the keypad takes over: the HMI sees that the network has lost control */
    g_sim[0].coils[1] = 1;
    run(14500, 14900);
    CHECK(tag(0, REMOTE) == 0);
    hmi_devmgr_shutdown();
}

/* EM6400NG behind a gateway: float registers scaled to tag units, read-only */
static void test_meter(void)
{
    enum { IA = 0, VAB = 3, VLL = 6, KW = 7, HZ = 8, KWH = 9 };
    world();
    CHECK(attach(SIM_METER, "em6400", 100));
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    run(100, 1200);
    hmi_devmgr_publish(d, 1200);
    CHECK(d[0].state == HMI_DEV_ONLINE && d[0].profile == &g_profile_em6400 && strcmp(d[0].label, "Energy meter 1") == 0);
    CHECK(tag(0, IA) == 512 && tag(0, VAB) == 4152 && tag(0, VLL) == 4151);  /* 5.12 A, 415.2 V, average 415.1 V */
    CHECK(tag(0, KW) > 320 && tag(0, KW) < 335 && tag(0, HZ) == 5000 && tag(0, KWH) == 1250);
    CHECK(g_t.tags[HMI_DEV_TAG(0, KW)].quality == QUALITY_GOOD && g_profile_em6400.n_actions == 0);
    /* the supply changes: the new values arrive */
    sim_nudge(&g_sim[0], 3, +1);
    run(1200, 1600);
    CHECK(tag(0, VAB) == 4202);
    hmi_devmgr_shutdown();
}

/* A device that stops answering goes offline and comes back */
static void test_link_loss(void)
{
    world();
    hmi_devmgr_init(60000);                              /* keep the offline device on the dashboard */
    CHECK(attach(SIM_ATV340, NULL, 100));
    run(100, 800);
    static hmi_device_info_t d[HMI_MAX_DEVICES];
    hmi_devmgr_publish(d, 800);
    CHECK(d[0].state == HMI_DEV_ONLINE);
    g_sim[0].freeze = true;
    run(800, 1800);
    hmi_devmgr_publish(d, 1800);
    CHECK(d[0].state == HMI_DEV_OFFLINE && d[0].timeouts >= 1 && tag(0, g_profile_altivar.comm_tag) == 1);
    CHECK(g_t.tags[HMI_DEV_TAG(0, 1)].quality == QUALITY_COMM_FAULT);
    g_sim[0].freeze = false;
    run(1800, 3500);
    hmi_devmgr_publish(d, 3500);
    CHECK(d[0].state == HMI_DEV_ONLINE && tag(0, g_profile_altivar.comm_tag) == 0);
    hmi_devmgr_shutdown();
}

int main(void)
{
    hmi_timer_init();                                    /* as app_init() does: the link timers need it */
    test_modules_match_devices();
    test_tank_scenario();
    test_hmi_heartbeat();
    test_stop_all_every_machine();
    test_eight_devices();
    test_device_overflow();
    test_reset_pulses();
    test_every_plc_comes_online();
    test_altivar_control();
    test_meter();
    test_link_loss();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
