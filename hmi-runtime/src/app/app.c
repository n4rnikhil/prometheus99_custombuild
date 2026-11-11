/**
 * @file app.c
 * @brief Portable HMI application core: fieldbus thread (1 kHz), UI frame logic (~30 Hz),
 *        input handling, rendering. No OS header is included here (see include/hal/).
 * @standards IEC 62443-4-2 (RBAC) / ISA-18.2 (alarms) / ISA-101 (display)
 *
 * Threads and ownership
 *   Fieldbus thread  owns the tag table, the alarm supervisor (and its journal),
 *                    the PLC drivers and a watchdog that supervises the UI heartbeat.
 *                    Publishes everything once per iteration through a seqlock.
 *   UI thread        (the platform shell's main thread) owns the renderer, RBAC context,
 *                    the persistent alarm log, and a watchdog that supervises the fieldbus
 *                    heartbeat. Reads only the latest seqlock snapshot (g_snap).
 *   UI -> fieldbus   operator commands as bits in one atomic word, and audit
 *                    events (role changes, access denials, ...) through a
 *                    single-producer/single-consumer ring.
 *
 * Plug and play
 *   A discovery thread (low priority) looks for PLCs; the fieldbus thread attaches
 *   each one to a free device slot with the matching driver module, polls it, and
 *   removes it from the dashboard after it has been offline for --remove-after-ms.
 *
 * Command line (the same options can be written in hmi.cfg next to the executable, which is
 * read first; # starts a comment)
 *   --plc <ip[:port][/unit][@module]>  a device to watch (attached even if it cannot identify
 *                              itself); @module forces a driver module, e.g. 192.168.1.30/u1@em6400
 *   --scan <spec>              extra discovery range, e.g. 192.168.1.0/24 or 10.0.0.5:502-510
 *   --no-discovery             only the --plc addresses, no scanning
 *   --config <path>            read options from this file instead of hmi.cfg;  --no-config: none
 *   --remove-after-ms <n>      offline devices leave the dashboard after n ms (0 = never)
 *   --max-devices <n>          attach at most n devices (default and maximum: HMI_MAX_DEVICES)
 *   --log <path>               alarm log file (default alarms.log next to the executable)
 *   --no-log                   disable the persistent alarm log
 *   --exit-after-ms <n>        close automatically after n ms (tests / measure.bat)
 *   --report <path>            write measured runtime statistics on exit
 *   --verify-log <path>        verify an alarm log's signature chain, print result, exit (0 = ok)
 *   --key <path>               device key file (default hmi.key in the protected folder, hal_secure_dir)
 *   --vault <path>             credential vault (default security.vault in the protected folder)
 *   --set-pin <role>           set a role's site PIN (asked for twice, never on the command line), exit
 *   --allow-factory-pins       BENCH ONLY: let roles sign in with the factory PINs (1111/2222/3333)
 *   --color-scheme <status|high-performance>   normal states green (default) or grey (ISA-101)
 *   --no-sound                 no alarm tone from this panel (a wired horn output still works)
 *   --emergency-sound <file>   sound looped during an EMERGENCY (default emergency.mp3 next to the program, else a built-in siren)
 *   --alarm-sound <file>       alarm sound instead of the two-tone (default alarm.mp3 next to the program)
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "app.h"
#include "config.h"
#include "core/seqlock.h"
#include "core/tags.h"
#include "core/alarms.h"
#include "core/alarm_log.h"
#include "core/watchdog.h"
#include "core/security.h"
#include "core/crc32.h"
#include "core/device_key.h"
#include "core/runtime_stats.h"
#include "renderer/micro_renderer.h"
#include "renderer/layout.h"
#include "fieldbus/device_manager.h"
#include "fieldbus/discovery.h"
#include "fieldbus/driver_host.h"
#include "hal/atomic.h"
#include "hal/fs.h"
#include "hal/gesture.h"
#include "hal/platform.h"
#include "hal/sys.h"
#include "hal/thread.h"
#include "hal/timer.h"

#ifndef HMI_STACK_FIELDBUS
#define HMI_STACK_FIELDBUS  (16u * 1024u)
#endif
#ifndef HMI_STACK_DISCOVERY
#define HMI_STACK_DISCOVERY (16u * 1024u)
#endif

#ifndef HMI_BUILD_ID
#define HMI_BUILD_ID "dev"
#endif
/* Embedded so `strings industrial_hmi.exe` identifies the build. */
const char g_hmi_build_info[] = "Prometheus Industrial HMI 2.0 build " HMI_BUILD_ID;

/* ------------------------------------------------------------------------- */
/* Shared state (fieldbus -> UI, seqlock protected; single writer)            */
/* ------------------------------------------------------------------------- */
typedef struct {
    hmi_tag_table_t        tags;
    hmi_alarm_supervisor_t alarms;
    hmi_watchdog_t         fb_watchdog;   /* fieldbus-side: supervises the UI heartbeat */
    hmi_fieldbus_stats_t   fb_stats;
    hmi_device_info_t      devices[HMI_MAX_DEVICES];
    uint16_t               ntags;         /* tags and alarm points in use: only these are copied */
    uint16_t               ndev;          /* device places in use: only these are copied */
} hmi_shared_state_t;

static hmi_shared_state_t g_shared;       /* written only by the fieldbus thread */
static hmi_seqlock_t      g_seqlock;
static hmi_shared_state_t g_snap;         /* UI thread's private copy */

/* Copies of the shared state touch only the tags, alarm points and device places in use, so the
 * memory reserved for the full capacity (HMI_MAX_DEVICES) stays out of the working set. */
static void copy_tags(hmi_tag_table_t *d, const hmi_tag_table_t *s, uint16_t ntags, uint16_t ndev)
{
    memcpy(d->tags, s->tags, (size_t)ntags * sizeof(s->tags[0]));
    memcpy(d->trend_samples, s->trend_samples, (size_t)ndev * sizeof(s->trend_samples[0]));
    memcpy(d->trend_head, s->trend_head, ndev);
    memcpy(d->trend_count, s->trend_count, ndev);
    d->global_dirty_mask = s->global_dirty_mask;
}

static void copy_alarms(hmi_alarm_supervisor_t *d, const hmi_alarm_supervisor_t *s, uint16_t npoints)
{
    size_t a = offsetof(hmi_alarm_supervisor_t, latch_state), b = offsetof(hmi_alarm_supervisor_t, journal);
    uint16_t jc = s->journal_count <= HMI_ALARM_JOURNAL_SIZE ? s->journal_count : HMI_ALARM_JOURNAL_SIZE;
    memcpy(d->points, s->points, (size_t)npoints * sizeof(s->points[0]));
    memcpy((char *)d + a, (const char *)s + a, b - a);                  /* the summary */
    memcpy(d->journal, s->journal, (size_t)jc * sizeof(s->journal[0])); /* records written so far */
    d->journal_head = s->journal_head;
    d->journal_count = jc;
    d->journal_total = s->journal_total;
}

static uint16_t tags_in_use(const hmi_alarm_supervisor_t *al)
{
    uint16_t n = hmi_tag_ids_in_use();
    if (al->points_in_use > n) n = al->points_in_use;
    return n > HMI_MAX_TAGS ? (uint16_t)HMI_MAX_TAGS : n;
}

/* Heartbeats (uptime ms), each written by one thread, read by the other */
static hal_atomic_t g_hb_ui_ms;
static hal_atomic_t g_hb_fieldbus_ms;
static hal_atomic_t g_running = 1;

/* UI -> fieldbus command bits */
#define CMD_BIT_ACK          (1 << 0)
#define CMD_BIT_SILENCE      (1 << 1)
#define CMD_BIT_ESTOP        (1 << 2)
#define CMD_BIT_ESTOP_RESET  (1 << 3)
#define CMD_BIT_EMERG_RESET  (1 << 4)
#define CMD_BIT_INJECT_FAULT (1 << 6)
#define CMD_BIT_WD_RESET     (1 << 7)
static hal_atomic_t g_cmd_bits;
/* EMERGENCY: set by the UI the moment the button is pressed, cleared by a confirmed Maintenance
 * reset. While it is set the fieldbus thread refuses every operator command, stops every machine
 * that appears or comes back online, and stops the HMI heartbeat so PLCs that watch it go safe. */
static hal_atomic_t g_emergency_on;
static hal_atomic_t g_emerg_unreached;   /* fieldbus -> UI: machines on the network not on this HMI */
/* Per-alarm acknowledge requests, one bit per tag (UI sets, fieldbus drains): lock-free and no
 * request is lost however fast the operator taps. */
#define ACK_WORDS ((HMI_MAX_TAGS + 31) / 32)
static hal_atomic_t g_ack_mask[ACK_WORDS];
/* UI -> fieldbus device commands (SPSC ring: UI produces, fieldbus consumes) */
#define DEV_CMD_RING 16
typedef struct { uint8_t slot, action; int16_t value; } dev_cmd_t;
static dev_cmd_t     g_dev_cmds[DEV_CMD_RING];
static hal_atomic_t  g_dev_cmd_head;   /* written by UI */
static hal_atomic_t  g_dev_cmd_tail;   /* written by fieldbus */

/* UI -> fieldbus alarm handling: shelve, unshelve, out of service (SPSC ring, already authorised) */
#define ALARM_OP_RING 16
enum { ALARM_OP_SHELVE = 1, ALARM_OP_UNSHELVE, ALARM_OP_OOS };
typedef struct { hmi_tag_id_t tag; uint8_t op; } alarm_op_t;
static alarm_op_t    g_alarm_ops[ALARM_OP_RING];
static hal_atomic_t  g_alarm_op_head;  /* written by UI */
static hal_atomic_t  g_alarm_op_tail;  /* written by fieldbus */

/* discovery -> fieldbus: found devices (SPSC ring) */
#define CAND_RING 8
static hmi_device_candidate_t g_cand[CAND_RING];
static hal_atomic_t g_cand_head;   /* written by the discovery thread */
static hal_atomic_t g_cand_tail;   /* written by the fieldbus thread */

/* UI -> fieldbus audit events (SPSC ring: UI produces, fieldbus consumes) */
#define UI_EVENT_RING 16
typedef struct {
    uint8_t severity;
    char    msg[41];
} ui_event_t;
static ui_event_t    g_ui_events[UI_EVENT_RING];
static hal_atomic_t g_ui_ev_head;   /* written by UI */
static hal_atomic_t g_ui_ev_tail;   /* written by fieldbus */
static hal_atomic_t g_ui_ev_dropped;

/* Host telemetry driver (set before the fieldbus thread starts; read-only afterwards) */
static const hmi_plc_driver_t *g_host_driver;

/* System alarm points owned by the runtime itself */
#define SYS_TAG_WATCHDOG 13
#define SYS_TAG_CAPACITY 14                      /* devices on the network that this HMI cannot show */
static const hmi_tag_meta_t s_system_meta[] = {
    { SYS_TAG_WATCHDOG, TAG_TYPE_DIGITAL, "HMI watchdog failover", "", 0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "A control loop stalled; check the HMI host for overload", "Maintenance: reset the failover latch in Settings", NULL },
    { SYS_TAG_CAPACITY, TAG_TYPE_DIGITAL, "Devices not shown", "", 0, 1, 0, 0, 0, ALARM_SEV_WARNING,
      "STOP ALL cannot reach them: stop at the machine", "Their addresses are in Alarms > History", NULL },
};
static bool     g_discovery_on = true;
static char     g_cfg_note[64];                  /* "Site config hmi.cfg: 6 options" for the journal */
static bool     g_have_plc_arg;
static uint32_t g_remove_after_ms = HMI_DEVICE_REMOVE_MS;

/* ------------------------------------------------------------------------- */
/* UI-thread state                                                            */
/* ------------------------------------------------------------------------- */
static hmi_renderer_t        g_renderer;
static hal_thread_t          g_fb_thread;
static hal_thread_t          g_disc_thread;
static hmi_security_ctx_t    g_security;
static hmi_watchdog_t        g_ui_wd;          /* UI-side: supervises the fieldbus heartbeat */
static hmi_gesture_tracker_t g_gesture;
static uint8_t               g_active_screen = SCREEN_PROCESS_OVERVIEW;
static uint32_t              g_seqlock_retries;
/* Data-to-screen latency probe: from the moment a device's read cycle lands in the fieldbus tag
 * table (hmi_device_info_t.data_us) to the end of the first frame drawn with that data. */
typedef struct {
    uint64_t seen_us[HMI_MAX_DEVICES];   /* data_us already counted, per slot */
    uint64_t win_sum_us;                 /* current 1 s window */
    uint32_t win_n, win_max_us;
    uint32_t win_start_ms;
    uint32_t avg_us, max_us;             /* last complete window (shown on Diagnostics) */
    uint64_t total_sum_us;               /* whole run (written to --report) */
    uint32_t total_n, total_max_us, over_target;
} latency_probe_t;
static latency_probe_t       g_latency;
static uint32_t              g_crc_mismatches;
static uint32_t              g_ui_frame_hz;
static uint32_t              g_exe_size;
static bool                  g_have_snapshot;
static char                  g_toast[72];
static uint32_t              g_toast_ms;
static bool                  g_log_enabled = true;
static char                  g_log_path[HMI_PATH_MAX];
static uint32_t              g_exit_after_ms;
static char                  g_report_path[HMI_PATH_MAX];
static char                  g_vault_path[HMI_PATH_MAX];
static char                  g_key_path[HMI_PATH_MAX];
static char                  g_verify_path[HMI_PATH_MAX];
static uint8_t               g_device_key[HMI_DEVICE_KEY_LEN];
static bool                  g_allow_factory_pins;
static bool                  g_high_performance;          /* --color-scheme high-performance */
static int                   g_set_pin_role = -1;
static uint32_t              g_ui_max_frame_ms;
static uint32_t              g_ui_wd_faults_seen;
static hmi_device_info_t     g_dev_view[HMI_MAX_DEVICES];   /* present devices, slot order */
static uint8_t               g_ndev;
static hmi_discovery_stats_t g_disc_stats;

/* Per-frame bookkeeping, shared by the main loop and the modal-loop timer (UI thread only) */
static uint32_t              g_ui_start_ms;
static uint32_t              g_ui_last_frame_ms;
static uint32_t              g_ui_last_beep_ms;
static uint32_t              g_ui_last_trim_ms;
static uint32_t              g_ui_fps_window_ms;
static uint32_t              g_ui_fps_frames;

#define TOAST_DURATION_MS 3000u

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */
static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && i + 1 < cap; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void ui_post_event(uint8_t severity, const char *msg)
{
    int32_t head = hal_atomic_load(&g_ui_ev_head);
    int32_t tail = hal_atomic_load(&g_ui_ev_tail);   /* atomic read */
    if ((uint32_t)(head - tail) >= UI_EVENT_RING) {
        hal_atomic_add(&g_ui_ev_dropped, 1);
        return;
    }
    ui_event_t *ev = &g_ui_events[(uint32_t)head % UI_EVENT_RING];
    ev->severity = severity;
    copy_str(ev->msg, sizeof(ev->msg), msg);
    hal_atomic_store(&g_ui_ev_head, head + 1);                   /* publish (full barrier) */
}

static void fb_drain_ui_events(hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int32_t tail = hal_atomic_load(&g_ui_ev_tail);
    int32_t head = hal_atomic_load(&g_ui_ev_head);
    while (tail != head) {
        const ui_event_t *ev = &g_ui_events[(uint32_t)tail % UI_EVENT_RING];
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ev->severity, ALARM_PT_NORMAL, ev->msg);
        tail++;
        hal_atomic_store(&g_ui_ev_tail, tail);
    }
    int32_t dropped = hal_atomic_exchange(&g_ui_ev_dropped, 0);
    if (dropped > 0) {
        char msg[41];
        snprintf(msg, sizeof(msg), "UI events dropped: %ld", (long)dropped);
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_WARNING, ALARM_PT_NORMAL, msg);
    }
}

static void ui_toast(const char *msg, uint32_t now_ms)
{
    copy_str(g_toast, sizeof(g_toast), msg);
    g_toast_ms = now_ms;
}

/* ------------------------------------------------------------------------- */
/* Fieldbus thread (1 kHz)                                                    */
/* ------------------------------------------------------------------------- */
typedef struct {
    uint64_t last_iter_us;
    uint64_t window_start_us;
    uint32_t window_count;
    uint64_t window_sum_us;
    uint32_t window_min_us;
    uint32_t window_max_us;
} loop_meter_t;

static void meter_tick(loop_meter_t *m, hmi_fieldbus_stats_t *st, uint64_t now_us)
{
    if (m->last_iter_us != 0) {
        uint64_t p = now_us - m->last_iter_us;
        uint32_t period = (p > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)p;
        m->window_sum_us += period;
        if (period < m->window_min_us) m->window_min_us = period;
        if (period > m->window_max_us) m->window_max_us = period;
        if (period > 2u * HMI_FIELDBUS_PERIOD_US) st->late_count++;
    }
    m->last_iter_us = now_us;
    m->window_count++;
    st->loop_count++;

    if (now_us - m->window_start_us >= 1000000u) {
        st->loop_hz = m->window_count;
        st->period_avg_us = m->window_count ? (uint32_t)(m->window_sum_us / m->window_count) : 0;
        st->period_min_us = (m->window_min_us == 0xFFFFFFFFu) ? 0 : m->window_min_us;
        st->period_max_us = m->window_max_us;
        m->window_start_us = now_us;
        m->window_count = 0;
        m->window_sum_us = 0;
        m->window_min_us = 0xFFFFFFFFu;
        m->window_max_us = 0;
    }
}

static void fb_handle_commands(int32_t bits, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms,
                               hmi_watchdog_t *wd, uint32_t now_ms, bool *stall_requested)
{
    if (bits & CMD_BIT_ESTOP) {
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_ESTOP, ALARM_PT_NORMAL,
                             "EMERGENCY pressed: software stop to every machine");
        uint8_t missed = 0;
        uint8_t stopped = hmi_devmgr_stop_all(tags, alarms, now_ms, &missed);
        char msg[57];
        if (missed) {
            snprintf(msg, sizeof(msg), "EMERGENCY: %u NOT stopped - use the hardwired E-stop", (unsigned)missed);
            hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_CRITICAL, ALARM_PT_NORMAL, msg);
        } else if (stopped == 0) {
            hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_WARNING, ALARM_PT_NORMAL,
                                 "EMERGENCY: no device to stop");
        } else {
            snprintf(msg, sizeof(msg), "EMERGENCY: stop sent to %u device(s)", (unsigned)stopped);
            hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_ESTOP, ALARM_PT_NORMAL, msg);
        }
    }
    if (bits & CMD_BIT_EMERG_RESET) {
        hmi_devmgr_stop_clear();
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL,
                             "EMERGENCY reset: machines stay stopped");
    }
    if (bits & CMD_BIT_ACK) {
        uint16_t n = hmi_alarms_acknowledge_all(alarms, now_ms);
        char msg[41];
        snprintf(msg, sizeof(msg), "Operator acknowledged %u alarm(s)", (unsigned)n);
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL, msg);
    }
    if (bits & CMD_BIT_SILENCE) {
        hmi_alarms_silence_horn(alarms);
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL, "Horn silenced");
    }
    if (bits & CMD_BIT_ESTOP_RESET) {
        if (hmi_devmgr_estop(false, tags, alarms, now_ms) == 0) {
            hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL,
                                 "Stop-latch reset: nothing to reset");
        }
    }
    if (bits & CMD_BIT_WD_RESET) {
        if (hmi_watchdog_reset_failover(wd)) hmi_tag_write_digital(tags, SYS_TAG_WATCHDOG, false, QUALITY_GOOD);
    }
    if (bits & CMD_BIT_INJECT_FAULT) {
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_WARNING, ALARM_PT_NORMAL,
                             "Watchdog test: fieldbus stalls 750 ms");
        *stall_requested = true;
    }
}

/* Device panel commands. Checked every iteration on their own: they must not depend on
 * a global command bit being set at the same moment. */
static void fb_handle_device_commands(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int32_t tail = hal_atomic_load(&g_dev_cmd_tail);
    int32_t head = hal_atomic_load(&g_dev_cmd_head);
    while (tail != head) {
        const dev_cmd_t *c = &g_dev_cmds[(uint32_t)tail % DEV_CMD_RING];
        hmi_devmgr_action(c->slot, c->action, c->value, tags, alarms, now_ms);
        tail++;
        hal_atomic_store(&g_dev_cmd_tail, tail);
    }
}

static void fb_handle_alarm_ops(hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int32_t tail = hal_atomic_load(&g_alarm_op_tail);
    int32_t head = hal_atomic_load(&g_alarm_op_head);
    while (tail != head) {
        const alarm_op_t *o = &g_alarm_ops[(uint32_t)tail % ALARM_OP_RING];
        if (o->op == ALARM_OP_SHELVE)        hmi_alarms_shelve(alarms, o->tag, HMI_ALARM_SHELVE_MS, now_ms);
        else if (o->op == ALARM_OP_UNSHELVE) hmi_alarms_unshelve(alarms, o->tag, now_ms);
        else if (o->op == ALARM_OP_OOS)      hmi_alarms_set_out_of_service(alarms, o->tag, true, now_ms);
        tail++;
        hal_atomic_store(&g_alarm_op_tail, tail);
    }
}

static void fb_discard_device_commands(hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int32_t tail = hal_atomic_load(&g_dev_cmd_tail);
    int32_t head = hal_atomic_load(&g_dev_cmd_head);
    if (tail == head) return;
    char msg[57];
    snprintf(msg, sizeof(msg), "EMERGENCY: %u operator command(s) cancelled", (unsigned)(uint32_t)(head - tail));
    hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_WARNING, ALARM_PT_NORMAL, msg);
    hal_atomic_store(&g_dev_cmd_tail, head);
}

/* Discovery thread -> fieldbus thread */
static void discovery_found(const hmi_device_candidate_t *c)
{
    int32_t head = hal_atomic_load(&g_cand_head);
    int32_t tail = hal_atomic_load(&g_cand_tail);
    if ((uint32_t)(head - tail) >= CAND_RING) return;          /* full: it will be found again next sweep */
    g_cand[(uint32_t)head % CAND_RING] = *c;
    hal_atomic_store(&g_cand_head, head + 1);
}

static void fb_attach_found(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int32_t tail = hal_atomic_load(&g_cand_tail);
    int32_t head = hal_atomic_load(&g_cand_head);
    while (tail != head) {
        hmi_devmgr_attach(&g_cand[(uint32_t)tail % CAND_RING], tags, alarms, now_ms);
        tail++;
        hal_atomic_store(&g_cand_tail, tail);
    }
}

/* Called by the discovery thread: is this address already on the dashboard? */
static bool discovery_attached(const char *host, uint16_t port, uint8_t unit)
{
    static hmi_device_info_t devs[HMI_MAX_DEVICES];
    uint32_t seq;
    uint16_t nd;
    do {
        seq = hmi_seqlock_read_begin(&g_seqlock);
        nd = g_shared.ndev <= HMI_MAX_DEVICES ? g_shared.ndev : (uint16_t)HMI_MAX_DEVICES;
        memcpy(devs, g_shared.devices, (size_t)nd * sizeof(devs[0]));
    } while (hmi_seqlock_read_retry(&g_seqlock, seq));
    char ep[24];
    snprintf(ep, sizeof(ep), "%s:%u", host, (unsigned)port);
    for (int i = 0; i < nd; i++) {
        if (devs[i].state != HMI_DEV_EMPTY && devs[i].transport == HMI_TRANSPORT_TCP &&
            devs[i].unit_id == unit && strcmp(devs[i].endpoint, ep) == 0) return true;
    }
    return false;
}

static void discovery_thread(void *param)
{
    (void)param;
    hmi_discovery_run(&g_running, discovery_attached, discovery_found);
}

static void fb_publish(const hmi_tag_table_t *tags, const hmi_alarm_supervisor_t *alarms,
                       const hmi_watchdog_t *wd, const hmi_fieldbus_stats_t *st, uint32_t now_ms)
{
    static hmi_device_info_t devs[HMI_MAX_DEVICES];
    uint16_t nd = (uint16_t)hmi_devmgr_publish_used(devs, now_ms);
    uint16_t nt = tags_in_use(alarms);
    hmi_seqlock_write_begin(&g_seqlock);
    copy_tags(&g_shared.tags, tags, nt, nd);
    copy_alarms(&g_shared.alarms, alarms, nt);
    g_shared.fb_watchdog = *wd;
    g_shared.fb_stats    = *st;
    memcpy(g_shared.devices, devs, (size_t)nd * sizeof(devs[0]));
    g_shared.ntags = nt;
    g_shared.ndev  = nd;
    hmi_seqlock_write_end(&g_seqlock);
}

static void fieldbus_thread(void *param)
{
    (void)param;

    static hmi_tag_table_t        tags;     /* static: keep ~4 KB off the thread stack */
    static hmi_alarm_supervisor_t alarms;
    static hmi_watchdog_t         wd;
    static hmi_fieldbus_stats_t   st;
    loop_meter_t meter;
    hmi_pacer_t  pacer;

    uint32_t now_ms = hmi_timer_get_ms();
    hmi_tags_init(&tags);
    hmi_alarms_init(&alarms);
    hmi_watchdog_init(&wd, now_ms);
    memset(&st, 0, sizeof(st));
    memset(&meter, 0, sizeof(meter));
    meter.window_min_us = 0xFFFFFFFFu;
    meter.window_start_us = hmi_timer_get_us();

    hmi_pacer_init(&pacer, HMI_FIELDBUS_PERIOD_US);
    st.high_res_timer = pacer.high_res;

    hmi_devmgr_init(g_remove_after_ms);
    hmi_alarms_log_event(&alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL,
                         "HMI started: plug-and-play");

    uint32_t last_trend_ms = now_ms - 1000u;
    uint32_t last_wd_ms = now_ms;
    uint32_t last_publish_ms = now_ms - HMI_PUBLISH_PERIOD_MS;
    uint32_t last_eval_ms = now_ms - HMI_ALARM_EVAL_MS;

    while (hal_atomic_load(&g_running)) {
        uint64_t now_us = hmi_timer_get_us();
        now_ms = (uint32_t)(now_us / 1000u);
        uint64_t gap_us = meter.last_iter_us ? now_us - meter.last_iter_us : 0;
        meter_tick(&meter, &st, now_us);
        if (gap_us > (uint64_t)HMI_WATCHDOG_MAX_AGE_MS * 1000u) {
            hmi_tag_write_digital(&tags, SYS_TAG_WATCHDOG, true, QUALITY_GOOD);   /* latched until reset */
        } else if (hmi_tag_read_quality(&tags, SYS_TAG_WATCHDOG) != QUALITY_GOOD) {
            hmi_tag_write_digital(&tags, SYS_TAG_WATCHDOG, false, QUALITY_GOOD);  /* first iteration */
        }
        {                                        /* machines on the network this HMI could not take on */
            bool over = hmi_devmgr_unattached(now_ms) > 0;
            if (hmi_tag_read_quality(&tags, SYS_TAG_CAPACITY) != QUALITY_GOOD ||
                (hmi_tag_read_value(&tags, SYS_TAG_CAPACITY) != 0) != over)
                hmi_tag_write_digital(&tags, SYS_TAG_CAPACITY, over, QUALITY_GOOD);
        }

        /* 1. Own heartbeat, and the UI heartbeat as seen from here */
        hal_atomic_store(&g_hb_fieldbus_ms, (int32_t)now_ms);
        hmi_watchdog_ping_fieldbus(&wd, now_ms);
        hmi_watchdog_ping_ui(&wd, (uint32_t)hal_atomic_load(&g_hb_ui_ms));

        /* 2. Operator commands (already authorised by the UI thread) */
        bool stall = false;
        int32_t bits = hal_atomic_exchange(&g_cmd_bits, 0);
        bool emergency = hal_atomic_load(&g_emergency_on) != 0;
        /* The HMI heartbeat to the PLCs only while the UI thread is alive and there is no EMERGENCY:
         * a frozen screen or an EMERGENCY must let the machines go to their safe state. */
        hmi_devmgr_heartbeat_enable(!emergency &&
                                    (uint32_t)(now_ms - (uint32_t)hal_atomic_load(&g_hb_ui_ms)) < HMI_WATCHDOG_MAX_AGE_MS);
        /* EMERGENCY overrides every operator command still waiting in the UI -> fieldbus ring: a Start
         * tapped a moment earlier must never reach a machine after the stop. */
        if ((bits & CMD_BIT_ESTOP) || emergency) fb_discard_device_commands(&alarms, now_ms);
        if (bits) fb_handle_commands(bits, &tags, &alarms, &wd, now_ms, &stall);
        /* While it lasts, every machine that appears or comes back online is stopped as well */
        if (emergency) hmi_devmgr_stop_pending(&alarms, now_ms);
        hal_atomic_store(&g_emerg_unreached, (int32_t)hmi_devmgr_unattached(now_ms));
        for (int w = 0; w < ACK_WORDS; w++) {
            uint32_t m = (uint32_t)hal_atomic_exchange(&g_ack_mask[w], 0);
            for (int b = 0; m && b < 32; b++) {
                if (!(m & (1u << b))) continue;
                m &= ~(1u << b);
                hmi_tag_id_t tag = (hmi_tag_id_t)(w * 32 + b);
                if (!hmi_alarms_acknowledge_point(&alarms, tag, now_ms)) continue;
                const hmi_tag_meta_t *meta = hmi_tag_get_meta(tag);
                char msg[57];
                snprintf(msg, sizeof(msg), "Operator acknowledged: %.20s %.12s",
                         meta && meta->group ? meta->group : "", meta && meta->name ? meta->name : "alarm");
                hmi_alarms_log_event(&alarms, now_ms, tag, ALARM_SEV_INFO, ALARM_PT_NORMAL, msg);
            }
        }
        fb_handle_device_commands(&tags, &alarms, now_ms);
        fb_handle_alarm_ops(&alarms, now_ms);

        /* 3. Cyclic I/O */
        fb_attach_found(&tags, &alarms, now_ms);
        hmi_devmgr_poll(&tags, &alarms, now_ms);
        if (g_host_driver && g_host_driver->poll_cyclic)     g_host_driver->poll_cyclic(&tags, now_ms);

        /* 4. 1 Hz trend: one per device, of the tag its driver nominates (wrap-safe) */
        if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) {
            last_trend_ms += 1000u;
            if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) last_trend_ms = now_ms;   /* after a stall */
            static hmi_device_info_t devs[HMI_MAX_DEVICES];
            int nd = hmi_devmgr_publish_used(devs, now_ms);
            for (uint8_t d = 0; d < nd; d++) {
                if (!hmi_device_present(&devs[d]) || devs[d].profile->trend_tag == HMI_REL_NONE) continue;
                hmi_tag_id_t id = HMI_DEV_TAG(d, devs[d].profile->trend_tag);
                uint8_t tq = hmi_tag_read_quality(&tags, id);
                hmi_tags_push_trend(&tags, d, (tq == QUALITY_GOOD || tq == QUALITY_SIMULATED)
                                              ? hmi_tag_read_value(&tags, id) : HMI_TREND_GAP);
            }
        }

        /* 5. Alarms, every HMI_ALARM_EVAL_MS (devices answer every 100 ms), and at once after a command */
        if (bits || (uint32_t)(now_ms - last_eval_ms) >= HMI_ALARM_EVAL_MS) {
            last_eval_ms = now_ms;
            hmi_alarms_evaluate(&alarms, &tags, now_ms);
        }

        /* 6. Supervise the UI loop */
        if ((uint32_t)(now_ms - last_wd_ms) >= HMI_WATCHDOG_TICK_MS) {
            last_wd_ms = now_ms;
            if (hmi_watchdog_evaluate(&wd, now_ms)) {
                hmi_alarms_log_event(&alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_CRITICAL, ALARM_PT_NORMAL,
                                     "Watchdog: UI loop stalled");
                hmi_tag_write_digital(&tags, SYS_TAG_WATCHDOG, true, QUALITY_GOOD);
            }
        }

        /* 7. Audit events from the UI thread */
        fb_drain_ui_events(&alarms, now_ms);

        /* 8. Publish every HMI_PUBLISH_PERIOD_MS, and at once after an operator command, so the
         *    UI never copies a large snapshot more often than it can use it. The CRC lets the UI
         *    verify the snapshot end to end. */
        if (bits || (uint32_t)(now_ms - last_publish_ms) >= HMI_PUBLISH_PERIOD_MS) {
            last_publish_ms = now_ms;
            st.publish_ms = now_ms;
            st.pacer_overruns = pacer.overruns;
            st.snapshot_crc = hmi_crc32c((const uint8_t *)tags.tags, (size_t)tags_in_use(&alarms) * sizeof(tags.tags[0]));
            fb_publish(&tags, &alarms, &wd, &st, now_ms);
        }

        if (stall) {
            /* Deliberate fault: stop heartbeating so the UI-side watchdog must catch it. */
            hmi_timer_sleep_ms(HMI_FAULT_INJECT_STALL_MS);
        }

        hmi_pacer_wait(&pacer);
    }

    hmi_devmgr_shutdown();
    hmi_pacer_close(&pacer);
}

/* ------------------------------------------------------------------------- */
/* UI: commands, input                                                        */
/* ------------------------------------------------------------------------- */
static void ui_request(hmi_command_t cmd, int32_t bit, uint32_t now_ms)
{
    if (!hmi_security_authorize(&g_security, cmd, now_ms)) {
        char msg[72];
        snprintf(msg, sizeof(msg), "Access denied: %s needs the %s role",
                 hmi_security_command_name(cmd), hmi_security_role_name(hmi_security_required_role(cmd)));
        ui_toast(msg, now_ms);
        char ev[41];
        snprintf(ev, sizeof(ev), "Denied: %s (%s)", hmi_security_command_name(cmd),
                 hmi_security_role_name(g_security.current_role));
        ui_post_event(ALARM_SEV_WARNING, ev);
        return;
    }
    if (bit) hal_atomic_or(&g_cmd_bits, bit);
}

/* --- Dialogs: sign-in, device faceplate, numeric keypad, confirmation ---
 * PIN digits live only here, never in the view. */
static uint8_t g_modal = HMI_MODAL_NONE;
static uint8_t g_modal_role;
static char    g_pin[HMI_PIN_MAX_LEN + 1];
static uint8_t g_pin_len;
static const char *g_modal_msg;
static bool    g_modal_msg_error;
static char    g_modal_msg_buf[80];

#define NO_SLOT 0xFFu
static uint8_t g_fp_slot = NO_SLOT;        /* faceplate open for this device (also behind nested dialogs) */
static uint8_t g_focus_slot = NO_SLOT;     /* Process screen: device view of this device (a tile was tapped) */

typedef struct {
    bool    active;
    uint8_t slot, action;
    int16_t value;
    bool    has_value;
    bool    system;          /* a runtime command (stop-latch reset), not a device action */
    uint8_t sys_cmd;         /* hmi_command_t when system */
    int32_t sys_bit;         /* CMD_BIT_* when system */
} ui_cmd_t;
static ui_cmd_t g_pending_login;           /* control waiting for a sign-in */
static ui_cmd_t g_pending_confirm;         /* command waiting for "Confirm" */

static char    g_np_entry[12], g_np_title[48], g_np_unit[12], g_np_info[96];
static uint8_t g_np_slot, g_np_action;
static char    g_cf_title[64], g_cf_detail[96], g_cf_ok[32];
static bool    g_cf_danger;

static void pin_clear(void)
{
    hal_secure_zero(g_pin, sizeof(g_pin));
    g_pin_len = 0;
}

static const hmi_device_info_t *ui_dev(uint8_t slot)
{
    return (slot < HMI_MAX_DEVICES && hmi_device_present(&g_snap.devices[slot])) ? &g_snap.devices[slot] : NULL;
}

/* Close the current dialog. Dialogs opened from a faceplate return to it. */
static void modal_close(void)
{
    pin_clear();
    g_modal_msg = NULL;
    g_modal_msg_error = false;
    g_pending_confirm.active = false;
    g_pending_login.active = false;
    if (g_modal != HMI_MODAL_FACEPLATE && ui_dev(g_fp_slot)) g_modal = HMI_MODAL_FACEPLATE;
    else { g_modal = HMI_MODAL_NONE; g_fp_slot = NO_SLOT; }
}

static void ui_open_role_dialog(uint32_t now_ms)
{
    if (!hmi_security_authorize(&g_security, HMI_CMD_ROLE_SELECT, now_ms)) return;
    pin_clear();
    g_fp_slot = NO_SLOT;
    g_modal = HMI_MODAL_ROLE;
    g_modal_msg = NULL;
}

static void ui_choose_role(uint8_t role, uint32_t now_ms)
{
    if (role >= HMI_ROLE_COUNT) return;
    if (role == (uint8_t)g_security.current_role) { modal_close(); return; }
    if (!hmi_security_role_needs_pin((hmi_role_t)role)) {
        hmi_security_login(&g_security, (hmi_role_t)role, NULL, now_ms);
        ui_post_event(ALARM_SEV_INFO, "Signed out: role set to Viewer");
        ui_toast("Signed out - Viewer (monitor only)", now_ms);
        modal_close();
        return;
    }
    g_modal_role = role;
    g_modal = HMI_MODAL_PIN;
    g_modal_msg = NULL;
    g_modal_msg_error = false;
    pin_clear();
}

static void ui_start_action(uint8_t slot, uint8_t action, uint8_t option, uint32_t now_ms);
static void ui_emergency_reset(uint32_t now_ms);
static void ui_alarm_op(uint8_t op, hmi_tag_id_t tag, uint32_t now_ms);
static void ui_open_faceplate(uint8_t slot);

static void ui_pin_submit(uint32_t now_ms)
{
    static char msg[48];
    hmi_role_t role = (hmi_role_t)g_modal_role;
    hmi_login_result_t res = hmi_security_login(&g_security, role, g_pin, now_ms);
    pin_clear();
    char ev[41];
    switch (res) {
        case HMI_LOGIN_OK: {
            snprintf(ev, sizeof(ev), "Signed in: %s", hmi_security_role_name(role));
            ui_post_event(ALARM_SEV_INFO, ev);
            snprintf(msg, sizeof(msg), "Signed in as %s", hmi_security_role_name(role));
            ui_toast(msg, now_ms);
            ui_cmd_t pend = g_pending_login;
            modal_close();
            if (pend.active && pend.system && pend.sys_cmd == HMI_CMD_EMERGENCY_RESET)
                ui_emergency_reset(now_ms);   /* carry on with the Reset that asked for the sign-in */
            else if (pend.active && pend.system &&
                     (pend.sys_cmd == HMI_CMD_SHELVE_ALARM || pend.sys_cmd == HMI_CMD_ALARM_SERVICE))
                ui_alarm_op(pend.action, (hmi_tag_id_t)pend.value, now_ms);   /* the shelve that asked */
            else if (pend.active && pend.system && pend.sys_cmd == HMI_CMD_ACK_ALARMS &&
                     (uint16_t)pend.value < HMI_MAX_TAGS) {                     /* the acknowledge that asked */
                hmi_tag_id_t t = (hmi_tag_id_t)pend.value;
                hal_atomic_or(&g_ack_mask[t / 32], (int32_t)(1u << (t % 32)));
            }
            else if (pend.active)   /* carry on with the control that asked for the sign-in */
                ui_start_action(pend.slot, pend.action, pend.has_value ? (uint8_t)pend.value : HMI_FP_NO_OPTION, now_ms);
            break;
        }
        case HMI_LOGIN_BAD_PIN:
            snprintf(ev, sizeof(ev), "Sign-in failed: %s", hmi_security_role_name(role));
            ui_post_event(ALARM_SEV_WARNING, ev);
            snprintf(msg, sizeof(msg), "Wrong PIN - %u attempt(s) left",
                     (unsigned)(HMI_PIN_MAX_ATTEMPTS - g_security.failed_attempts));
            g_modal_msg = msg;
            g_modal_msg_error = true;
            break;
        case HMI_LOGIN_LOCKED:
            ui_post_event(ALARM_SEV_WARNING, "PIN entry locked for 30 s");
            g_modal_msg = NULL;
            break;
        case HMI_LOGIN_FACTORY_PIN:
            snprintf(ev, sizeof(ev), "Factory PIN refused: %s", hmi_security_role_name(role));
            ui_post_event(ALARM_SEV_WARNING, ev);
            g_modal_msg = "Factory PIN: set a site PIN first";
            g_modal_msg_error = true;
            break;
        default:
            g_modal_msg = "Enter 4 to 8 digits";
            g_modal_msg_error = true;
            break;
    }
}

static void ui_pin_key(uint8_t key, uint32_t now_ms)
{
    if (hmi_security_lockout_remaining_ms(&g_security, now_ms) > 0) return;
    if (key <= 9) {
        if (g_pin_len < HMI_PIN_MAX_LEN) { g_pin[g_pin_len++] = (char)('0' + key); g_pin[g_pin_len] = '\0'; }
        if (!g_pending_login.active) g_modal_msg = NULL;
        g_modal_msg_error = false;
    } else if (key == HMI_KEY_DELETE) {
        if (g_pin_len) g_pin[--g_pin_len] = '\0';
    } else if (key == HMI_KEY_OK) {
        ui_pin_submit(now_ms);
    }
}

/* --- Operator controls ----------------------------------------------------- */

static void fmt_with_unit(char *out, size_t cap, int32_t raw, const hmi_tag_meta_t *m)
{
    char num[16];
    hmi_format_scaled(num, sizeof(num), (int16_t)raw, m ? m->decimals : 0);
    snprintf(out, cap, "%s%s%s", num, (m && m->unit && m->unit[0]) ? " " : "", (m && m->unit) ? m->unit : "");
}

/* "35.5" with 1 decimal -> 355. false = not a number, or more decimals than the tag has. */
static bool parse_scaled(const char *t, uint8_t dec, int32_t *out)
{
    bool neg = false, dot = false, any = false;
    int32_t ip = 0, fp = 0;
    int fd = 0;
    if (*t == '-') { neg = true; t++; }
    for (; *t; t++) {
        if (*t == '.') { if (dot) return false; dot = true; continue; }
        if (*t < '0' || *t > '9') return false;
        any = true;
        if (dot) { if (++fd > dec) return false; fp = fp * 10 + (*t - '0'); }
        else { ip = ip * 10 + (*t - '0'); if (ip > 99999) return false; }
    }
    if (!any) return false;
    int32_t scale = 1;
    for (uint8_t i = 0; i < dec; i++) scale *= 10;
    while (fd < dec) { fp *= 10; fd++; }
    *out = neg ? -(ip * scale + fp) : ip * scale + fp;
    return true;
}

static bool ui_queue_device_cmd(uint8_t slot, uint8_t action, int16_t value, uint32_t now_ms)
{
    int32_t head = hal_atomic_load(&g_dev_cmd_head);
    int32_t tail = hal_atomic_load(&g_dev_cmd_tail);
    if ((uint32_t)(head - tail) >= DEV_CMD_RING) { ui_toast("Too many commands at once - try again", now_ms); return false; }
    dev_cmd_t *c = &g_dev_cmds[(uint32_t)head % DEV_CMD_RING];
    c->slot = slot;
    c->action = action;
    c->value = value;
    hal_atomic_store(&g_dev_cmd_head, head + 1);                     /* publish */
    return true;
}

static void ui_send(const ui_cmd_t *c, uint32_t now_ms)
{
    const hmi_device_info_t *d = ui_dev(c->slot);
    if (!d || c->action >= d->profile->n_actions) return;
    const hmi_device_action_t *a = &d->profile->actions[c->action];
    if (!ui_queue_device_cmd(c->slot, c->action, c->value, now_ms)) return;
    char msg[72], v[32];
    if (a->kind == HMI_ACT_SET_REG) {
        fmt_with_unit(v, sizeof(v), c->value, hmi_tag_get_meta(HMI_DEV_TAG(c->slot, a->rel_tag)));
        snprintf(msg, sizeof(msg), "%s -> %s sent", a->log_name, v);
    } else if (a->kind == HMI_ACT_SET_ENUM) {
        hmi_action_option_label(a, (uint8_t)c->value, v, sizeof(v));
        snprintf(msg, sizeof(msg), "%s -> %s sent", a->log_name, v);
    } else {
        snprintf(msg, sizeof(msg), "%s: %s sent", a->log_name, a->label);
    }
    ui_toast(msg, now_ms);
}

static void ui_ask_confirm(const ui_cmd_t *c)
{
    const hmi_device_info_t *d = ui_dev(c->slot);
    if (!d) return;
    const hmi_device_action_t *a = &d->profile->actions[c->action];
    char v[32];
    if (a->kind == HMI_ACT_SET_REG) {
        fmt_with_unit(v, sizeof(v), c->value, hmi_tag_get_meta(HMI_DEV_TAG(c->slot, a->rel_tag)));
        snprintf(g_cf_title, sizeof(g_cf_title), "Set %s to %s?", a->log_name, v);
        snprintf(g_cf_ok, sizeof(g_cf_ok), "Set");
    } else if (a->kind == HMI_ACT_SET_ENUM) {
        hmi_action_option_label(a, (uint8_t)c->value, v, sizeof(v));
        snprintf(g_cf_title, sizeof(g_cf_title), "Switch %s to %s?", a->log_name, v);
        snprintf(g_cf_ok, sizeof(g_cf_ok), "Switch to %.20s", v);
    } else {
        snprintf(g_cf_title, sizeof(g_cf_title), "%s: %s?", a->log_name, a->label);
        snprintf(g_cf_ok, sizeof(g_cf_ok), "%s", a->label);
    }
    if (a->command == HMI_CMD_RAW_WRITE)
        snprintf(g_cf_detail, sizeof(g_cf_detail), "%s  -  writes holding register %u directly", d->label, (unsigned)a->address);
    else
        snprintf(g_cf_detail, sizeof(g_cf_detail), "%s  -  %s", d->label, d->endpoint);
    g_cf_danger = hmi_act_is_stop(a->kind);      /* red only for stop actions */
    g_pending_confirm = *c;
    g_pending_confirm.active = true;
    g_modal = HMI_MODAL_CONFIRM;
}

/* Confirmation for a runtime command that is not tied to one device. */
static void ui_ask_confirm_system(hmi_command_t cmd, int32_t bit, const char *title, const char *detail,
                                  const char *ok, uint32_t now_ms)
{
    /* Too low a role: ui_request() refuses, counts and reports it once. Checked again on confirm. */
    if (g_security.current_role < hmi_security_required_role(cmd)) { ui_request(cmd, bit, now_ms); return; }
    snprintf(g_cf_title, sizeof(g_cf_title), "%s", title);
    snprintf(g_cf_detail, sizeof(g_cf_detail), "%s", detail);
    snprintf(g_cf_ok, sizeof(g_cf_ok), "%s", ok);
    g_cf_danger = false;                         /* red means stop; a reset is not a stop */
    memset(&g_pending_confirm, 0, sizeof(g_pending_confirm));
    g_pending_confirm.active = true;
    g_pending_confirm.system = true;
    g_pending_confirm.slot = NO_SLOT;
    g_pending_confirm.sys_cmd = (uint8_t)cmd;
    g_pending_confirm.sys_bit = bit;
    g_modal = HMI_MODAL_CONFIRM;
}

/* --- EMERGENCY ---------------------------------------------------------------
 * Anyone may press it, in every state, dialog or not: software stop to every machine, then the
 * whole HMI turns red and stays locked until Maintenance resets it after a confirmation. The reset
 * only leaves the red screen: nothing restarts. The hardwired E-stops stay the safety function. */
static bool     g_emergency;
static uint32_t g_emergency_at_ms;

static void ui_emergency_press(uint32_t now_ms)
{
    hal_atomic_store(&g_emergency_on, 1);                 /* first: no command may slip through */
    ui_request(HMI_CMD_ESTOP, CMD_BIT_ESTOP, now_ms);     /* never refused */
    if (!g_emergency) {
        g_emergency = true;
        g_emergency_at_ms = now_ms;
        char ev[41];
        snprintf(ev, sizeof(ev), "EMERGENCY pressed (role: %s)", hmi_security_role_name(g_security.current_role));
        ui_post_event(ALARM_SEV_ESTOP, ev);
    }
    g_fp_slot = NO_SLOT;                                  /* close every dialog: nothing else may run */
    modal_close();
}

static void ui_emergency_reset(uint32_t now_ms)
{
    if (!g_emergency) return;
    hmi_role_t need = hmi_security_required_role(HMI_CMD_EMERGENCY_RESET);
    if (g_security.current_role < need) {                 /* ask for the PIN, then come back here */
        pin_clear();
        g_modal_role = (uint8_t)need;
        g_modal = HMI_MODAL_PIN;
        snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "%s sign-in needed: Reset EMERGENCY",
                 hmi_security_role_name(need));
        g_modal_msg = g_modal_msg_buf;
        g_modal_msg_error = false;
        memset(&g_pending_login, 0, sizeof(g_pending_login));
        g_pending_login.active = true;
        g_pending_login.system = true;
        g_pending_login.slot = NO_SLOT;
        g_pending_login.sys_cmd = (uint8_t)HMI_CMD_EMERGENCY_RESET;
        g_pending_login.sys_bit = CMD_BIT_EMERG_RESET;
        return;
    }
    ui_ask_confirm_system(HMI_CMD_EMERGENCY_RESET, CMD_BIT_EMERG_RESET, "Reset the EMERGENCY?",
                          "Only when the area is safe. Machines stay stopped.",
                          "Reset EMERGENCY", now_ms);
}

/* --- Alarm dialog: acknowledge, shelve, out of service, open the device --- */
static hmi_tag_id_t g_almd_tag;

static void ui_open_alarm(hmi_tag_id_t tag)
{
    if (tag >= HMI_MAX_TAGS || !hmi_tag_get_meta(tag)) return;
    pin_clear();
    g_almd_tag = tag;
    g_modal = HMI_MODAL_ALARM;
    g_modal_msg = NULL;
}

/* Shelve / unshelve (Operator) or out of service / return (Maintenance). Asks for the PIN of the
 * role it needs and carries on after the sign-in. The fieldbus thread applies it and journals it. */
static void ui_alarm_op(uint8_t op, hmi_tag_id_t tag, uint32_t now_ms)
{
    if (tag >= HMI_MAX_TAGS) return;
    const hmi_alarm_point_t *pt = &g_snap.alarms.points[tag];
    bool service = op == ALARM_OP_OOS || (op == ALARM_OP_UNSHELVE && (pt->flags & HMI_ALARM_F_OOS) &&
                                          !(pt->flags & HMI_ALARM_F_SHELVED));
    hmi_command_t cmd = service ? HMI_CMD_ALARM_SERVICE : HMI_CMD_SHELVE_ALARM;
    hmi_role_t need = hmi_security_required_role(cmd);
    if (g_security.current_role < need) {
        pin_clear();
        g_modal_role = (uint8_t)need;
        g_modal = HMI_MODAL_PIN;
        snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "%s sign-in needed: %s",
                 hmi_security_role_name(need), hmi_security_command_name(cmd));
        g_modal_msg = g_modal_msg_buf;
        g_modal_msg_error = false;
        memset(&g_pending_login, 0, sizeof(g_pending_login));
        g_pending_login.active = true;
        g_pending_login.system = true;
        g_pending_login.slot = NO_SLOT;
        g_pending_login.sys_cmd = (uint8_t)cmd;
        g_pending_login.action = op;
        g_pending_login.value = (int16_t)tag;
        return;
    }
    if (!hmi_security_authorize(&g_security, cmd, now_ms)) return;
    if (op != ALARM_OP_UNSHELVE && !hmi_alarms_can_hide(tag)) {
        ui_toast("A stop alarm cannot be shelved or taken out of service", now_ms);
        return;
    }
    int32_t head = hal_atomic_load(&g_alarm_op_head);
    if ((uint32_t)(head - hal_atomic_load(&g_alarm_op_tail)) >= ALARM_OP_RING) {
        ui_toast("Busy - try again", now_ms);
        return;
    }
    g_alarm_ops[(uint32_t)head % ALARM_OP_RING] = (alarm_op_t){ tag, op };
    hal_atomic_store(&g_alarm_op_head, head + 1);
    {                                                     /* who did it, for the audit log */
        char ev[41];
        snprintf(ev, sizeof(ev), "%s by %s", op == ALARM_OP_SHELVE ? "Shelve" : op == ALARM_OP_OOS ? "Out of service"
                 : "Unshelve", hmi_security_role_name(g_security.current_role));
        ui_post_event(ALARM_SEV_INFO, ev);
    }
    char msg[64];
    if (op == ALARM_OP_SHELVE)
        snprintf(msg, sizeof(msg), "Alarm shelved for %lu min", (unsigned long)(HMI_ALARM_SHELVE_MS / 60000u));
    else
        snprintf(msg, sizeof(msg), op == ALARM_OP_OOS ? "Alarm taken out of service" : "Alarm shown again");
    ui_toast(msg, now_ms);
    if (g_modal == HMI_MODAL_ALARM) modal_close();
}

static void ui_alarm_button(uint8_t button, uint32_t now_ms)
{
    hmi_tag_id_t tag = g_almd_tag;
    if (tag >= HMI_MAX_TAGS) return;
    const hmi_alarm_point_t *pt = &g_snap.alarms.points[tag];
    switch (button) {
        case HMI_ALMD_ACK: {
            if (pt->state != ALARM_PT_UNACK && pt->state != ALARM_PT_RTN_UNACK) return;
            hmi_role_t need = hmi_security_required_role(HMI_CMD_ACK_ALARMS);
            if (g_security.current_role < need) {                     /* ask for the PIN, then ack */
                pin_clear();
                g_modal_role = (uint8_t)need;
                g_modal = HMI_MODAL_PIN;
                snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "%s sign-in needed: Acknowledge",
                         hmi_security_role_name(need));
                g_modal_msg = g_modal_msg_buf;
                g_modal_msg_error = false;
                memset(&g_pending_login, 0, sizeof(g_pending_login));
                g_pending_login.active = true;
                g_pending_login.system = true;
                g_pending_login.slot = NO_SLOT;
                g_pending_login.sys_cmd = (uint8_t)HMI_CMD_ACK_ALARMS;
                g_pending_login.value = (int16_t)tag;
                return;
            }
            ui_request(HMI_CMD_ACK_ALARMS, 0, now_ms);                 /* audit of the authorisation */
            hal_atomic_or(&g_ack_mask[tag / 32], (int32_t)(1u << (tag % 32)));
            if (g_modal == HMI_MODAL_ALARM) modal_close();
            break;
        }
        case HMI_ALMD_SHELVE:
            if (pt->flags & HMI_ALARM_F_OOS) return;
            ui_alarm_op((pt->flags & HMI_ALARM_F_SHELVED) ? ALARM_OP_UNSHELVE : ALARM_OP_SHELVE, tag, now_ms);
            break;
        case HMI_ALMD_SERVICE:
            if (pt->flags & HMI_ALARM_F_OOS) {
                if (pt->flags & HMI_ALARM_F_SHELVED) return;           /* unshelve first */
                ui_alarm_op(ALARM_OP_UNSHELVE, tag, now_ms);
            } else {
                ui_alarm_op(ALARM_OP_OOS, tag, now_ms);
            }
            break;
        case HMI_ALMD_DEVICE:
            if (tag >= HMI_DEVICE_TAG_BASE)
                ui_open_faceplate((uint8_t)((tag - HMI_DEVICE_TAG_BASE) / HMI_DEVICE_TAG_SPAN));
            break;
        default: break;
    }
}

static void ui_confirmed(const ui_cmd_t *c, uint32_t now_ms)
{
    if (c->system && c->sys_cmd == HMI_CMD_EMERGENCY_RESET) {
        if (!g_emergency) return;
        ui_request(HMI_CMD_EMERGENCY_RESET, 0, now_ms);   /* role check (and denial report) */
        if (g_security.current_role < hmi_security_required_role(HMI_CMD_EMERGENCY_RESET)) return;
        g_emergency = false;
        hal_atomic_store(&g_emergency_on, 0);
        hal_atomic_or(&g_cmd_bits, CMD_BIT_EMERG_RESET);
        char ev[41];
        snprintf(ev, sizeof(ev), "EMERGENCY reset by %s", hmi_security_role_name(g_security.current_role));
        ui_post_event(ALARM_SEV_INFO, ev);
        ui_toast("EMERGENCY reset - machines stay stopped", now_ms);
        return;
    }
    if (c->system) ui_request((hmi_command_t)c->sys_cmd, c->sys_bit, now_ms);
    else ui_send(c, now_ms);
}

static void ui_open_numpad(uint8_t slot, uint8_t action)
{
    const hmi_device_info_t *d = ui_dev(slot);
    if (!d) return;
    const hmi_device_action_t *a = &d->profile->actions[action];
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(slot, a->rel_tag));
    char now_v[24], lo[24], hi[24];
    fmt_with_unit(now_v, sizeof(now_v), g_snap.tags.tags[HMI_DEV_TAG(slot, a->rel_tag)].value, m);
    hmi_format_scaled(lo, sizeof(lo), a->min, m ? m->decimals : 0);
    hmi_format_scaled(hi, sizeof(hi), a->max, m ? m->decimals : 0);
    g_np_slot = slot;
    g_np_action = action;
    g_np_entry[0] = '\0';
    snprintf(g_np_title, sizeof(g_np_title), "%s", a->log_name);
    snprintf(g_np_unit, sizeof(g_np_unit), "%s", (m && m->unit) ? m->unit : "");
    snprintf(g_np_info, sizeof(g_np_info), "Now %s   -   allowed %s to %s", now_v, lo, hi);
    g_modal_msg = NULL;
    g_modal_msg_error = false;
    g_modal = HMI_MODAL_NUMPAD;
}

static void ui_open_faceplate(uint8_t slot)
{
    if (!ui_dev(slot)) return;
    pin_clear();
    g_fp_slot = slot;
    g_modal = HMI_MODAL_FACEPLATE;
    g_modal_msg = NULL;
}

/* Every operator control goes through here:
 * device online -> role (asks for the PIN it needs) -> interlock -> value entry -> confirmation -> send. */
static void ui_start_action(uint8_t slot, uint8_t action, uint8_t option, uint32_t now_ms)
{
    const hmi_device_info_t *d = ui_dev(slot);
    if (!d || action >= d->profile->n_actions) return;
    const hmi_device_action_t *a = &d->profile->actions[action];
    char msg[72];
    if (d->state != HMI_DEV_ONLINE) {
        snprintf(msg, sizeof(msg), "%s is not online: command not sent", d->label);
        ui_toast(msg, now_ms);
        return;
    }
    hmi_role_t need = hmi_security_required_role((hmi_command_t)a->command);
    if (g_security.current_role < need) {
        pin_clear();
        g_modal_role = (uint8_t)need;
        g_modal = HMI_MODAL_PIN;
        snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "%s sign-in needed: %s", hmi_security_role_name(need), a->log_name);
        g_modal_msg = g_modal_msg_buf;
        g_modal_msg_error = false;
        g_pending_login.active = true;
        g_pending_login.slot = slot;
        g_pending_login.action = action;
        g_pending_login.has_value = option != HMI_FP_NO_OPTION;
        g_pending_login.value = option;
        return;
    }
    if (hmi_act_is_start(a->kind) && a->block_tag != HMI_REL_NONE &&
        g_snap.tags.tags[HMI_DEV_TAG(slot, a->block_tag)].value != 0) {
        const hmi_tag_meta_t *bm = hmi_tag_get_meta(HMI_DEV_TAG(slot, a->block_tag));
        snprintf(msg, sizeof(msg), "%s blocked: %s is active", a->label, bm ? bm->name : "interlock");
        ui_toast(msg, now_ms);
        return;
    }
    if (a->kind == HMI_ACT_SET_REG) { ui_open_numpad(slot, action); return; }
    ui_cmd_t c = { true, slot, action, 0, false, false, 0, 0 };
    if (a->kind == HMI_ACT_SET_ENUM) {
        if (option == HMI_FP_NO_OPTION) { ui_open_faceplate(slot); return; }   /* pick the option there */
        c.value = option;
        c.has_value = true;
        if (g_snap.tags.tags[HMI_DEV_TAG(slot, a->rel_tag)].value == option) return;   /* already selected */
    }
    if (a->confirm) { ui_ask_confirm(&c); return; }
    ui_send(&c, now_ms);
}

static void ui_numpad_key(uint8_t key, uint32_t now_ms)
{
    const hmi_device_info_t *d = ui_dev(g_np_slot);
    if (!d) { modal_close(); return; }
    const hmi_device_action_t *a = &d->profile->actions[g_np_action];
    const hmi_tag_meta_t *m = hmi_tag_get_meta(HMI_DEV_TAG(g_np_slot, a->rel_tag));
    uint8_t dec = m ? m->decimals : 0;
    size_t n = strlen(g_np_entry);
    if (key <= 9) {
        if (n < 9) { g_np_entry[n] = (char)('0' + key); g_np_entry[n + 1] = '\0'; }
        g_modal_msg = NULL;
    } else if (key == HMI_NP_DOT) {
        if (dec > 0 && !strchr(g_np_entry, '.') && n < 8) {
            if (n == 0 || (n == 1 && g_np_entry[0] == '-')) { g_np_entry[n++] = '0'; }
            g_np_entry[n] = '.'; g_np_entry[n + 1] = '\0';
        }
    } else if (key == HMI_NP_MINUS) {
        if (n == 0 && a->min < 0) { g_np_entry[0] = '-'; g_np_entry[1] = '\0'; }
    } else if (key == HMI_NP_DEL) {
        if (n) g_np_entry[n - 1] = '\0';
        g_modal_msg = NULL;
    } else if (key == HMI_NP_OK) {
        int32_t v;
        char lo[24], hi[24];
        hmi_format_scaled(lo, sizeof(lo), a->min, dec);
        hmi_format_scaled(hi, sizeof(hi), a->max, dec);
        if (!parse_scaled(g_np_entry, dec, &v)) {
            if (dec) snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "Enter a number with up to %u decimal(s)", (unsigned)dec);
            else snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "Enter a whole number");
            g_modal_msg = g_modal_msg_buf;
            g_modal_msg_error = true;
            return;
        }
        if (v < a->min || v > a->max) {
            snprintf(g_modal_msg_buf, sizeof(g_modal_msg_buf), "Out of range: allowed %s to %s %s", lo, hi, (m && m->unit) ? m->unit : "");
            g_modal_msg = g_modal_msg_buf;
            g_modal_msg_error = true;
            return;
        }
        ui_cmd_t c = { true, g_np_slot, g_np_action, (int16_t)v, true, false, 0, 0 };
        if (a->confirm) { ui_ask_confirm(&c); return; }
        ui_send(&c, now_ms);
        modal_close();
    }
}

static void ui_toggle_theme(void)
{
    g_security.high_contrast_theme = !g_security.high_contrast_theme;
    hmi_renderer_set_theme(&g_renderer, g_security.high_contrast_theme);
}

static void ui_navigate(int delta)
{
    int s = ((int)g_active_screen + delta + SCREEN_COUNT) % SCREEN_COUNT;
    g_active_screen = (uint8_t)s;
}

static void ui_watchdog_reset(uint32_t now_ms)
{
    if (!hmi_security_authorize(&g_security, HMI_CMD_WATCHDOG_RESET, now_ms)) {
        ui_request(HMI_CMD_WATCHDOG_RESET, 0, now_ms);   /* reports the denial */
        return;
    }
    bool ok = hmi_watchdog_reset_failover(&g_ui_wd);
    hal_atomic_or(&g_cmd_bits, CMD_BIT_WD_RESET);
    ui_post_event(ALARM_SEV_INFO, ok ? "Watchdog failover reset" : "Watchdog reset refused: not healthy");
    ui_toast(ok ? "Watchdog failover latch reset" : "Watchdog reset refused: loop not healthy", now_ms);
}

static const uint32_t* ui_active_tokens(uint8_t *count)
{
    *count = 0;
    if (g_active_screen == SCREEN_HOST_TELEMETRY && g_host_driver) {
        *count = g_host_driver->widget_count;
        return g_host_driver->ui_bytecode;
    }
    return NULL;
}

/* A button on a device panel: the control it stands for in the device's current state. */
static void ui_device_action(hmi_tag_id_t abs_tag, uint32_t now_ms)
{
    if (abs_tag < HMI_DEVICE_TAG_BASE) return;
    uint8_t slot = (uint8_t)((abs_tag - HMI_DEVICE_TAG_BASE) / HMI_DEVICE_TAG_SPAN);
    uint8_t rel  = (uint8_t)((abs_tag - HMI_DEVICE_TAG_BASE) % HMI_DEVICE_TAG_SPAN);
    const hmi_device_info_t *d = ui_dev(slot);
    if (!d) return;
    int idx = hmi_profile_quick_action(d->profile, rel, g_snap.tags.tags[abs_tag].value);
    if (idx >= 0) ui_start_action(slot, (uint8_t)idx, HMI_FP_NO_OPTION, now_ms);
}

/* Keyboard: P / V = quick control of faceplate row 0 / 1 of the first online device; O = its faceplate */
static void ui_first_device_row(int row, uint32_t now_ms)
{
    for (uint8_t d = 0; d < g_ndev; d++) {
        const hmi_device_profile_t *p = g_dev_view[d].profile;
        if (g_dev_view[d].state != HMI_DEV_ONLINE || !p) continue;
        if (row < 0) { ui_open_faceplate(g_dev_view[d].slot); return; }
        uint8_t rels[HMI_FP_MAX_ROWS];
        uint8_t n = hmi_profile_fp_rows(p, rels);
        if (row < n) { ui_device_action(HMI_DEV_TAG(g_dev_view[d].slot, rels[row]), now_ms); return; }
    }
    ui_toast("No connected device has that control", now_ms);
}

static uint8_t  g_alarm_view;              /* Alarms screen: Active summary or History */
static uint16_t g_alarm_page;
static uint16_t g_dev_page;                      /* process screen page (more devices than fit) */

/* Readings screen: every reading of every device, built once per frame while the screen is shown.
 * Row list = per device a heading and its readings that pass the filter and the search. When a
 * page starts inside a device, the heading is repeated ("continued"); a heading never ends a page. */
#define READ_ROWS_MAX (2 * (HMI_MAX_TAGS + HMI_MAX_DEVICES + 1))
static uint8_t        g_read_filter;           /* HMI_READ_FILTER_* */
static uint16_t       g_read_page, g_read_pages = 1;
static char           g_read_query[24];        /* upper case */
static hmi_read_row_t g_read_rows[READ_ROWS_MAX];
static uint16_t       g_read_n, g_read_matches, g_read_total;
static int            g_read_per = 1;          /* rows per page, from the layout */

static bool ui_banner_active(void)
{
    return g_have_snapshot && hmi_layout_banner_shown(g_active_screen, g_snap.alarms.current_action.active);
}

static int ui_alarm_pages(void);
static hmi_tag_id_t g_alarm_order[HMI_MAX_TAGS]; /* active alarms in display order, this frame */
static uint16_t g_alarm_order_n;
static hmi_tag_id_t g_hidden[HMI_MAX_TAGS];      /* shelved / out-of-service points, this frame */
static uint16_t g_hidden_n;

/* Case-insensitive "does text contain the (upper-case) query" */
static bool read_contains(const char *text, const char *q)
{
    if (!q[0]) return true;
    if (!text) return false;
    for (const char *s = text; *s; s++) {
        size_t i = 0;
        while (q[i]) {
            char c = s[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != q[i]) break;
            i++;
        }
        if (!q[i]) return true;
    }
    return false;
}

static bool read_passes(hmi_tag_id_t id, const hmi_tag_meta_t *m, const hmi_device_info_t *d, bool group_hit)
{
    if (g_read_filter == HMI_READ_FILTER_ALARM && g_snap.alarms.points[id].state == ALARM_PT_NORMAL) return false;
    if (g_read_filter == HMI_READ_FILTER_BAD) {
        uint8_t q = g_snap.tags.tags[id].quality;
        bool good = (q == QUALITY_GOOD || q == QUALITY_SIMULATED) && (!d || d->state == HMI_DEV_ONLINE);
        if (good) return false;
    }
    return group_hit || read_contains(m->name, g_read_query) || read_contains(m->unit, g_read_query);
}

/* Fewer rows than this per page: no headings; each row names its device instead */
#define READ_FLAT_BELOW 6

static void read_push(uint8_t kind, uint8_t slot, hmi_tag_id_t tag)
{
    if (g_read_n + 2u >= READ_ROWS_MAX) return;
    if (g_read_per < READ_FLAT_BELOW && kind != HMI_READ_ROW_TAG) return;
    if (g_read_per >= READ_FLAT_BELOW) {
        int pos = g_read_n % g_read_per;
        if (kind == HMI_READ_ROW_TAG && pos == 0 && g_read_n > 0)
            g_read_rows[g_read_n++] = (hmi_read_row_t){ HMI_READ_ROW_CONTINUED, slot, 0 };
        else if (kind == HMI_READ_ROW_DEVICE && pos == g_read_per - 1)
            g_read_rows[g_read_n++] = (hmi_read_row_t){ HMI_READ_ROW_BLANK, slot, 0 };
    }
    g_read_rows[g_read_n++] = (hmi_read_row_t){ kind, slot, tag };
}

/* One group: a device (d != NULL) or this HMI's own readings (ids first..last). */
static void read_group(const hmi_device_info_t *d, hmi_tag_id_t first, hmi_tag_id_t count)
{
    uint8_t slot = d ? d->slot : HMI_READ_SLOT_HMI;
    bool group_hit = g_read_query[0] &&
        (d ? (read_contains(d->label, g_read_query) || read_contains(d->product, g_read_query) ||
              read_contains(d->endpoint, g_read_query))
           : read_contains("THIS HMI", g_read_query));
    bool heading = false;
    for (hmi_tag_id_t id = first; id < first + count; id++) {
        const hmi_tag_meta_t *m = hmi_tag_get_meta(id);
        if (!m) continue;
        g_read_total++;
        if (!read_passes(id, m, d, group_hit)) continue;
        g_read_matches++;
        if (!heading) { read_push(HMI_READ_ROW_DEVICE, slot, 0); heading = true; }
        read_push(HMI_READ_ROW_TAG, slot, id);
    }
}

static void ui_build_readings(void)
{
    hmi_rect_t card = hmi_layout_read_card(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active());
    g_read_per = hmi_layout_read_rows(card);
    g_read_n = g_read_matches = g_read_total = 0;
    if (g_have_snapshot) {
        for (uint8_t i = 0; i < g_ndev; i++)
            read_group(&g_dev_view[i], HMI_DEV_TAG(g_dev_view[i].slot, 0), HMI_DEVICE_TAG_SPAN);
        read_group(NULL, 0, HMI_DEVICE_TAG_BASE);
    }
    g_read_pages = g_read_n ? (uint16_t)((g_read_n + g_read_per - 1) / g_read_per) : 1u;
    if (g_read_page >= g_read_pages) g_read_page = (uint16_t)(g_read_pages - 1u);
}

static void ui_search_key(char c)
{
    size_t n = strlen(g_read_query);
    if (c == HMI_KBD_DONE) { modal_close(); return; }
    if (c == HMI_KBD_DEL) { if (n) g_read_query[n - 1] = '\0'; }
    else if (c == HMI_KBD_CLEAR) g_read_query[0] = '\0';
    else if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == ' ' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
        if (n + 1 < sizeof(g_read_query) && !(c == ' ' && n == 0)) { g_read_query[n] = c; g_read_query[n + 1] = '\0'; }
    }
    g_read_page = 0;
}

static void ui_dispatch_hit(const hmi_hit_t *hit, uint32_t now_ms)
{
    /* EMERGENCY screen: only EMERGENCY, Reset and the dialogs Reset opens accept input */
    if (g_emergency && g_modal == HMI_MODAL_NONE && hit->kind != HMI_HIT_ESTOP && hit->kind != HMI_HIT_EMERGENCY_RESET)
        return;
    switch (hit->kind) {
        case HMI_HIT_NAV_TAB:
            if (hit->param == SCREEN_PROCESS_OVERVIEW) g_focus_slot = NO_SLOT;   /* Process again: all devices */
            if (hit->param < SCREEN_COUNT) g_active_screen = (uint8_t)hit->param;
            break;
        case HMI_HIT_DEVICE_FOCUS:  if (ui_dev((uint8_t)hit->param)) g_focus_slot = (uint8_t)hit->param; break;
        case HMI_HIT_FOCUS_BACK:    g_focus_slot = NO_SLOT; break;
        case HMI_HIT_ESTOP:         ui_emergency_press(now_ms); break;
        case HMI_HIT_EMERGENCY_RESET: ui_emergency_reset(now_ms); break;
        case HMI_HIT_THEME:
        case HMI_HIT_SET_THEME:     ui_toggle_theme(); break;
        case HMI_HIT_ROLE:
        case HMI_HIT_SET_ROLE:      ui_open_role_dialog(now_ms); break;
        case HMI_HIT_MODAL_ROLE:    ui_choose_role((uint8_t)hit->param, now_ms); break;
        case HMI_HIT_MODAL_KEY:     ui_pin_key((uint8_t)hit->param, now_ms); break;
        case HMI_HIT_MODAL_CANCEL:
            if (g_modal == HMI_MODAL_FACEPLATE) g_fp_slot = NO_SLOT;    /* Close: leave the faceplate */
            modal_close();
            break;
        case HMI_HIT_MODAL_BLOCK:   break;
        case HMI_HIT_DEVICE_OPEN:   ui_open_faceplate((uint8_t)hit->param); break;
        case HMI_HIT_FP_CTRL: {
            const hmi_device_info_t *d = ui_dev(g_fp_slot);
            if (!d) break;
            hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
            uint8_t n = hmi_profile_fp_controls(d->profile, cs, HMI_FP_MAX_CTRLS);
            if (hit->param < n) ui_start_action(g_fp_slot, cs[hit->param].action, cs[hit->param].option, now_ms);
            break;
        }
        case HMI_HIT_NUMPAD_KEY:    ui_numpad_key((uint8_t)hit->param, now_ms); break;
        case HMI_HIT_CONFIRM_OK:
            if (g_pending_confirm.active) {
                ui_cmd_t c = g_pending_confirm;
                modal_close();
                ui_confirmed(&c, now_ms);
            }
            break;
        case HMI_HIT_BANNER_ACK:
        case HMI_HIT_ALARM_ACK:     ui_request(HMI_CMD_ACK_ALARMS, CMD_BIT_ACK, now_ms); break;
        case HMI_HIT_ALARM_SILENCE: ui_request(HMI_CMD_SILENCE_HORN, CMD_BIT_SILENCE, now_ms); break;
        case HMI_HIT_ALARM_TAB:
            if (hit->param < HMI_ALARM_VIEWS && hit->param != g_alarm_view) { g_alarm_view = (uint8_t)hit->param; g_alarm_page = 0; }
            break;
        case HMI_HIT_ALMD_BTN:      ui_alarm_button((uint8_t)hit->param, now_ms); break;
        case HMI_HIT_DEVICE_PAGE:
        {
            hmi_rect_t c = hmi_layout_content(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active());
            int per = hmi_layout_devices_per_page(c, g_ndev);
            int pages = per > 0 ? (g_ndev + per - 1) / per : 1;
            if (g_dev_page >= pages) g_dev_page = (uint16_t)(pages - 1);
            if (hit->param == 0 && g_dev_page > 0) g_dev_page--;
            else if (hit->param == 1 && g_dev_page + 1 < pages) g_dev_page++;
            break;
        }
        case HMI_HIT_READ_FILTER:
            if (hit->param < HMI_READ_FILTERS) { g_read_filter = (uint8_t)hit->param; g_read_page = 0; }
            break;
        case HMI_HIT_READ_SEARCH:   g_modal = HMI_MODAL_SEARCH; break;
        case HMI_HIT_KBD_KEY:       ui_search_key(hmi_layout_kbd_char(hit->param)); break;
        case HMI_HIT_READ_PAGE:
            if (hit->param == 0 && g_read_page > 0) g_read_page--;
            else if (hit->param == 1 && g_read_page + 1u < g_read_pages) g_read_page++;
            break;
        case HMI_HIT_READ_ROW: {
            uint32_t k = (uint32_t)g_read_page * (uint32_t)g_read_per + hit->param;
            if (hit->param < g_read_per && k < g_read_n && g_read_rows[k].kind != HMI_READ_ROW_BLANK &&
                g_read_rows[k].slot != HMI_READ_SLOT_HMI)
                ui_open_faceplate(g_read_rows[k].slot);
            break;
        }
        case HMI_HIT_ALARM_PAGE:
        {
            int pages = ui_alarm_pages();
            if (g_alarm_page >= pages) g_alarm_page = (uint16_t)(pages - 1);
            if (hit->param == 0 && g_alarm_page > 0) g_alarm_page--;
            else if (hit->param == 1 && g_alarm_page + 1 < pages) g_alarm_page++;
            break;
        }
        case HMI_HIT_ESTOP_RESET:
            /* ISO 13850 4.1.3: resetting is a deliberate act and must not restart the machine. */
            ui_ask_confirm_system(HMI_CMD_ESTOP_RESET, CMD_BIT_ESTOP_RESET, "Reset the stop latch?",
                                  "Check the machine is clear. Reset does not restart anything.", "Reset latch", now_ms);
            break;
        case HMI_HIT_SET_WD_TEST:   ui_request(HMI_CMD_INJECT_FAULT, CMD_BIT_INJECT_FAULT, now_ms); break;
        case HMI_HIT_SET_WD_RESET:  ui_watchdog_reset(now_ms); break;
        case HMI_HIT_WIDGET_BUTTON:
            ui_device_action(hit->param, now_ms);
            break;
        case HMI_HIT_NONE:
        default:
            break;
    }
}

/* Alarms screen: an Ack button on one row acknowledges that alarm only (ISA-18.2). The rows are
 * the same points, in the same order and geometry, as the renderer draws them. */
/* Pages of the current Alarms view, with the same geometry the renderer uses. */
static int ui_alarm_pages(void)
{
    if (!g_have_snapshot) return 1;
    hmi_rect_t ca = hmi_layout_alarm_active_card(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active(), 0);
    int n, per;
    if (g_alarm_view == HMI_ALARM_VIEW_ACTIVE) {
        n = g_alarm_order_n;                     /* sorted in ui_build_view, this frame */
        per = hmi_layout_alarm_rows(ca);
    } else if (g_alarm_view == HMI_ALARM_VIEW_SHELVED) {
        n = g_hidden_n;
        per = hmi_layout_alarm_rows(ca);
    } else {
        n = hmi_alarms_journal_count(&g_snap.alarms);
        per = hmi_layout_history_rows(ca);
    }
    return n ? (n + per - 1) / per : 1;
}

/* Alarms screen rows: the row button (Ack, or Unshelve / Return on the Shelved view) acts on that
 * alarm only; a tap anywhere else on the row opens the alarm dialog. The rows are the same
 * points, in the same order and geometry, as the renderer draws them (sorted this frame). */
static bool ui_alarm_row_tap(int x, int y, uint32_t now_ms)
{
    if (g_modal != HMI_MODAL_NONE || g_active_screen != SCREEN_ALARM_MATRIX || !g_have_snapshot) return false;
    if (g_alarm_view == HMI_ALARM_VIEW_HISTORY) return false;
    const hmi_alarm_supervisor_t *al = &g_snap.alarms;
    bool shelved_view = g_alarm_view == HMI_ALARM_VIEW_SHELVED;
    const hmi_tag_id_t *list = shelved_view ? g_hidden : g_alarm_order;
    uint16_t n = shelved_view ? g_hidden_n : g_alarm_order_n;
    if (!n) return false;
    hmi_rect_t ca = hmi_layout_alarm_active_card(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active(), n);
    int per = hmi_layout_alarm_rows(ca);
    int pages = (n + per - 1) / per;
    int page = g_alarm_page < pages ? g_alarm_page : pages - 1;
    for (int row = 0; row < per; row++) {
        int k = page * per + row;
        if (k >= n) break;
        hmi_tag_id_t i = list[k];
        uint8_t st = al->points[i].state;
        if (hmi_rect_contains(hmi_layout_alarm_row_ack(ca, row), x, y)) {
            if (shelved_view) { ui_alarm_op(ALARM_OP_UNSHELVE, i, now_ms); return true; }
            if (st != ALARM_PT_UNACK && st != ALARM_PT_RTN_UNACK) return true;   /* nothing to acknowledge */
            ui_request(HMI_CMD_ACK_ALARMS, 0, now_ms);                         /* role check, denial report */
            if (g_security.current_role >= hmi_security_required_role(HMI_CMD_ACK_ALARMS))
                hal_atomic_or(&g_ack_mask[i / 32], (int32_t)(1u << (i % 32)));
            return true;
        }
        if (hmi_rect_contains(hmi_layout_alarm_row(ca, row), x, y)) { ui_open_alarm(i); return true; }
    }
    return false;
}

static void ui_handle_key(uint16_t key, bool shift, uint32_t now_ms)
{
    /* EMERGENCY key: works in every state, dialog or not, except while the search keyboard is open,
     * where a PC keyboard types letters (E included). The on-screen EMERGENCY button stays live above
     * that dialog as above every other one. */
    if (key == 'E' && g_modal != HMI_MODAL_SEARCH) {
        ui_emergency_press(now_ms);
        return;
    }
    if (g_emergency && g_modal == HMI_MODAL_NONE) return;   /* the red screen takes no other keys */
    if (g_modal != HMI_MODAL_NONE) {      /* a dialog owns the keyboard */
        if (key == HMI_K_ESC) {
            if (g_modal == HMI_MODAL_FACEPLATE) g_fp_slot = NO_SLOT;
            modal_close();
            return;
        }
        if (g_modal == HMI_MODAL_ROLE && key >= '1' && key <= '4') { ui_choose_role((uint8_t)(key - '1'), now_ms); return; }
        if (g_modal == HMI_MODAL_PIN) {
            if (key >= '0' && key <= '9') ui_pin_key((uint8_t)(key - '0'), now_ms);
            else if (key == HMI_K_BACK) ui_pin_key(HMI_KEY_DELETE, now_ms);
            else if (key == HMI_K_ENTER) ui_pin_key(HMI_KEY_OK, now_ms);
        } else if (g_modal == HMI_MODAL_NUMPAD) {
            if (key >= '0' && key <= '9') ui_numpad_key((uint8_t)(key - '0'), now_ms);
            else if (key == '.') ui_numpad_key(HMI_NP_DOT, now_ms);
            else if (key == '-') ui_numpad_key(HMI_NP_MINUS, now_ms);
            else if (key == HMI_K_BACK) ui_numpad_key(HMI_NP_DEL, now_ms);
            else if (key == HMI_K_ENTER) ui_numpad_key(HMI_NP_OK, now_ms);
        } else if (g_modal == HMI_MODAL_SEARCH) {
            if (key == HMI_K_BACK) ui_search_key(HMI_KBD_DEL);
            else if (key == HMI_K_ENTER) ui_search_key(HMI_KBD_DONE);
            else if (key < 128) ui_search_key((char)key);
        } else if (g_modal == HMI_MODAL_CONFIRM && key == HMI_K_ENTER && g_pending_confirm.active) {
            ui_cmd_t c = g_pending_confirm;
            modal_close();
            ui_confirmed(&c, now_ms);
        }
        return;
    }
    switch (key) {
        case HMI_K_TAB:    ui_navigate(shift ? -1 : 1); break;
        case HMI_K_RIGHT:  ui_navigate(1); break;
        case HMI_K_LEFT:   ui_navigate(-1); break;
        case '1': case '2': case '3': case '4': case '5': case '6':
            g_active_screen = (uint8_t)(key - '1');
            break;
        case ' ':
        case 'A':       ui_request(HMI_CMD_ACK_ALARMS, CMD_BIT_ACK, now_ms); break;
        case 'S':       ui_request(HMI_CMD_SILENCE_HORN, CMD_BIT_SILENCE, now_ms); break;
        case 'P':       ui_first_device_row(0, now_ms); break;
        case 'V':       ui_first_device_row(1, now_ms); break;
        case 'O':       ui_first_device_row(-1, now_ms); break;

        case 'X':       ui_request(HMI_CMD_ESTOP_RESET, CMD_BIT_ESTOP_RESET, now_ms); break;
        case 'C':
        case 'T':       ui_toggle_theme(); break;
        case 'R':
        case 'L':       ui_open_role_dialog(now_ms); break;
        case 'F':       ui_request(HMI_CMD_INJECT_FAULT, CMD_BIT_INJECT_FAULT, now_ms); break;
        case 'W':       ui_watchdog_reset(now_ms); break;
        default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* UI: frame                                                                  */
/* ------------------------------------------------------------------------- */
static void ui_take_snapshot(void)
{
    uint32_t seq;
    uint32_t tries = 0;
    do {
        seq = hmi_seqlock_read_begin(&g_seqlock);
        uint16_t nt = g_shared.ntags <= HMI_MAX_TAGS ? g_shared.ntags : (uint16_t)HMI_MAX_TAGS;
        uint16_t nd = g_shared.ndev <= HMI_MAX_DEVICES ? g_shared.ndev : (uint16_t)HMI_MAX_DEVICES;
        copy_tags(&g_snap.tags, &g_shared.tags, nt, nd);
        copy_alarms(&g_snap.alarms, &g_shared.alarms, nt);
        g_snap.fb_watchdog = g_shared.fb_watchdog;
        g_snap.fb_stats = g_shared.fb_stats;
        memcpy(g_snap.devices, g_shared.devices, (size_t)nd * sizeof(g_snap.devices[0]));
        g_snap.ntags = nt;
        g_snap.ndev = nd;
        tries++;
    } while (hmi_seqlock_read_retry(&g_seqlock, seq));
    g_seqlock_retries += tries - 1u;

    if (g_snap.fb_stats.publish_ms != 0 || g_snap.fb_stats.loop_count != 0) {
        g_have_snapshot = true;
        uint32_t crc = hmi_crc32c((const uint8_t *)g_snap.tags.tags, (size_t)g_snap.ntags * sizeof(g_snap.tags.tags[0]));
        if (crc != g_snap.fb_stats.snapshot_crc) g_crc_mismatches++;
    }

    g_ndev = 0;
    for (uint8_t i = 0; i < g_snap.ndev; i++) {
        if (hmi_device_present(&g_snap.devices[i])) g_dev_view[g_ndev++] = g_snap.devices[i];
    }
}

static void ui_build_view(hmi_view_t *v, uint32_t now_ms)
{
    static hmi_alarm_log_stats_t log_stats;
    memset(v, 0, sizeof(*v));
    hmi_alarm_log_get_stats(&log_stats);

    v->tags   = g_have_snapshot ? &g_snap.tags : NULL;
    v->alarms = g_have_snapshot ? &g_snap.alarms : NULL;
    v->sec    = &g_security;
    v->wd     = &g_ui_wd;
    v->fb     = g_have_snapshot ? &g_snap.fb_stats : NULL;
    v->log    = &log_stats;
    v->devices = g_dev_view;
    v->ndev    = g_ndev;
    hmi_discovery_get_stats(&g_disc_stats);
    v->disc    = &g_disc_stats;
    if (g_host_driver)   { v->tokens_host = g_host_driver->ui_bytecode;      v->ntok_host = g_host_driver->widget_count; }
    v->active_screen   = g_active_screen;
    v->ui_wd_alive     = g_have_snapshot ? g_snap.fb_watchdog.ui_alive : true;
    v->snapshot_age_ms = g_have_snapshot ? (uint32_t)(now_ms - g_snap.fb_stats.publish_ms) : 0;
    v->data_stale      = !g_have_snapshot || v->snapshot_age_ms > HMI_SNAPSHOT_STALE_MS;
    v->crc_mismatches  = g_crc_mismatches;
    v->seqlock_retries = g_seqlock_retries;
    v->latency_avg_us  = g_latency.avg_us;
    v->latency_max_us  = g_latency.max_us;
    v->ui_frame_hz     = g_ui_frame_hz;
    v->exe_size_bytes  = g_exe_size;
    v->now_ms          = now_ms;
    v->utc_ms          = hal_utc_ms();
    v->tz_offset_s     = hal_local_offset_s();
    v->alarm_view      = g_alarm_view;
    g_alarm_order_n    = g_have_snapshot ? hmi_alarms_sorted(&g_snap.alarms, g_alarm_order, HMI_MAX_TAGS) : 0;
    v->alarm_order     = g_have_snapshot ? g_alarm_order : NULL;
    v->alarm_order_n   = g_alarm_order_n;
    g_hidden_n         = (g_have_snapshot && g_snap.alarms.shelved_count)
                         ? hmi_alarms_hidden(&g_snap.alarms, g_hidden, HMI_MAX_TAGS) : 0;
    v->hidden          = g_hidden;
    v->hidden_n        = g_hidden_n;
    v->almd_tag        = g_almd_tag;
    {                                            /* alarms can clear while a later page is shown */
        int pages = ui_alarm_pages();
        if (g_alarm_page >= pages) g_alarm_page = (uint16_t)(pages - 1);
    }
    v->alarm_page      = g_alarm_page;
    {                                            /* devices can leave while a later page is shown */
        int per = hmi_layout_devices_per_page(hmi_layout_content(g_renderer.screen_w, g_renderer.screen_h,
                                                                 ui_banner_active()), g_ndev);
        int pages = per > 0 ? (g_ndev + per - 1) / per : 1;
        if (g_dev_page >= pages) g_dev_page = (uint16_t)(pages - 1);
    }
    v->device_page     = g_dev_page;
    if (g_focus_slot != NO_SLOT && !ui_dev(g_focus_slot)) g_focus_slot = NO_SLOT;   /* the device left */
    v->focus_dev       = ui_dev(g_focus_slot);
    if (g_active_screen == SCREEN_READINGS) {   /* only while it is shown: up to ~1,600 readings */
        ui_build_readings();
        uint32_t first = (uint32_t)g_read_page * (uint32_t)g_read_per;
        v->read_rows    = g_read_rows + first;
        v->read_nrows   = (uint16_t)(g_read_n > first ? (g_read_n - first < (uint32_t)g_read_per ? g_read_n - first
                                                                                                 : (uint32_t)g_read_per) : 0u);
        v->read_page    = g_read_page;
        v->read_pages   = g_read_pages;
        v->read_matches = g_read_matches;
        v->read_total   = g_read_total;
        v->read_filter  = g_read_filter;
        v->read_flat    = g_read_per < READ_FLAT_BELOW;
    }
    v->read_query = g_read_query;
    v->toast = (g_toast[0] && (uint32_t)(now_ms - g_toast_ms) < TOAST_DURATION_MS) ? g_toast : NULL;
    v->modal = g_modal;
    v->modal_role = g_modal_role;
    v->pin_len = g_pin_len;
    v->lockout_ms = hmi_security_lockout_remaining_ms(&g_security, now_ms);
    v->modal_msg = g_modal_msg;
    v->modal_msg_error = g_modal_msg_error;
    v->fp_slot   = g_fp_slot;
    v->np_title  = g_np_title;
    v->np_entry  = g_np_entry;
    v->np_unit   = g_np_unit;
    v->np_info   = g_np_info;
    v->cf_title  = g_cf_title;
    v->cf_detail = g_cf_detail;
    v->cf_ok     = g_cf_ok;
    v->cf_danger = g_cf_danger;
    v->emergency = g_emergency;
    v->emergency_at_ms = g_emergency_at_ms;
    v->emergency_unreached = (uint8_t)hal_atomic_load(&g_emerg_unreached);
}

/* Called once the whole frame has been handed to the platform shell: every device whose data
 * changed since the last frame gives one latency sample. */
static void latency_sample(uint32_t now_ms)
{
    if (!g_have_snapshot) return;
    uint64_t now_us = hmi_timer_get_us();
    for (int d = 0; d < g_snap.ndev; d++) {
        uint64_t t = g_snap.devices[d].data_us;
        if (!t || t == g_latency.seen_us[d] || !hmi_device_present(&g_snap.devices[d])) continue;
        g_latency.seen_us[d] = t;
        uint32_t us = now_us > t ? (uint32_t)(now_us - t) : 0u;
        g_latency.win_sum_us += us;
        g_latency.win_n++;
        if (us > g_latency.win_max_us) g_latency.win_max_us = us;
        g_latency.total_sum_us += us;
        g_latency.total_n++;
        if (us > g_latency.total_max_us) g_latency.total_max_us = us;
        if (us > HMI_LATENCY_TARGET_MS * 1000u) g_latency.over_target++;
    }
    if ((uint32_t)(now_ms - g_latency.win_start_ms) >= 1000u) {
        g_latency.avg_us = g_latency.win_n ? (uint32_t)(g_latency.win_sum_us / g_latency.win_n) : 0u;
        g_latency.max_us = g_latency.win_max_us;
        g_latency.win_sum_us = 0;
        g_latency.win_n = g_latency.win_max_us = 0;
        g_latency.win_start_ms = now_ms;
    }
}

void app_render(app_present_fn present, void *ctx)
{
    static hmi_view_t view;
    uint32_t now_ms = hmi_timer_get_ms();
    ui_build_view(&view, now_ms);
    const uint32_t *palette = hmi_renderer_get_palette(&g_renderer);

    for (int b = 0; b < g_renderer.band_count; b++) {
        hmi_renderer_render_band(&g_renderer, b, &view);
        int rows = g_renderer.band_h;
        if (g_renderer.current_band_y + rows > g_renderer.screen_h) rows = g_renderer.screen_h - g_renderer.current_band_y;
        if (rows <= 0) break;
        present(ctx, hmi_renderer_get_band_buffer(&g_renderer), g_renderer.current_band_y, rows,
                g_renderer.screen_w, palette);
    }
    latency_sample(now_ms);
}

/* ------------------------------------------------------------------------- */
/* UI: one frame of housekeeping                                              */
/* ------------------------------------------------------------------------- */
/* Heartbeat, snapshot, watchdog / RBAC supervision, horn, audit-log sync and a
 * repaint request. app_tick() calls it every HMI_UI_FRAME_PERIOD_MS; a shell whose OS
 * runs its own loop for a while (Windows window drag / resize) must keep calling it, or the
 * UI heartbeat stops and the fieldbus-side watchdog raises a false CRITICAL failover. */
void app_frame(uint32_t now_ms)
{
    uint32_t since = now_ms - g_ui_last_frame_ms;
    if (since > g_ui_max_frame_ms && g_ui_fps_frames > 2) g_ui_max_frame_ms = since;
    g_ui_last_frame_ms = now_ms;
    g_ui_fps_frames++;
    if ((uint32_t)(now_ms - g_ui_fps_window_ms) >= 1000u) {
        g_ui_frame_hz = g_ui_fps_frames;
        g_ui_fps_frames = 0;
        g_ui_fps_window_ms = now_ms;
    }

    /* Heartbeat + snapshot */
    hal_atomic_store(&g_hb_ui_ms, (int32_t)now_ms);
    ui_take_snapshot();

    /* A device that left the dashboard takes its dialogs with it */
    bool fp_gone = g_fp_slot != NO_SLOT && !ui_dev(g_fp_slot);
    bool np_gone = g_modal == HMI_MODAL_NUMPAD && !ui_dev(g_np_slot);
    bool cf_gone = g_modal == HMI_MODAL_CONFIRM && !g_pending_confirm.system && !ui_dev(g_pending_confirm.slot);
    if (fp_gone || np_gone || cf_gone) {
        g_fp_slot = NO_SLOT;
        if (g_modal == HMI_MODAL_FACEPLATE || g_modal == HMI_MODAL_NUMPAD || g_modal == HMI_MODAL_CONFIRM) {
            g_modal = HMI_MODAL_NONE;
            g_pending_confirm.active = false;
            ui_toast("The device left the dashboard: dialog closed", now_ms);
        }
    }

    /* Supervise the fieldbus loop from this (independent) thread */
    hmi_watchdog_ping_ui(&g_ui_wd, now_ms);
    hmi_watchdog_ping_fieldbus(&g_ui_wd, (uint32_t)hal_atomic_load(&g_hb_fieldbus_ms));
    if (hmi_watchdog_evaluate(&g_ui_wd, now_ms)) {
        g_ui_wd_faults_seen++;
        ui_post_event(ALARM_SEV_CRITICAL, "Watchdog: fieldbus loop stalled");
        ui_toast("Watchdog tripped: fieldbus loop stalled, data frozen", now_ms);
    }

    /* RBAC inactivity logout */
    if (hmi_security_check_timeout(&g_security, now_ms)) {
        ui_post_event(ALARM_SEV_INFO, "Auto-logout: role set to Viewer");
        ui_toast("Inactivity: role dropped to Viewer", now_ms);
    }

    /* Horn: a steady output for a wired buzzer / beacon, and 1 s pulses on the panel itself.
     * While an EMERGENCY is latched its own sound takes over until the reset. */
    hal_emergency_sound(g_emergency);
    hal_horn(g_have_snapshot && g_snap.alarms.horn_sounding);
    if (g_have_snapshot && g_snap.alarms.horn_sounding && (uint32_t)(now_ms - g_ui_last_beep_ms) >= 1000u) {
        g_ui_last_beep_ms = now_ms;
        hal_beep();
    }

    /* Persistent audit log */
    if (g_have_snapshot) hmi_alarm_log_sync(&g_snap.alarms);

    /* Working-set trim every 10 s */
    if ((uint32_t)(now_ms - g_ui_last_trim_ms) >= 10000u) {
        g_ui_last_trim_ms = now_ms;
        hal_trim_memory();
    }

    hal_request_repaint();

    if (g_exit_after_ms && (uint32_t)(now_ms - g_ui_start_ms) >= g_exit_after_ms) hal_request_quit();
}

/* ------------------------------------------------------------------------- */
/* Input from the platform shell                                              */
/* ------------------------------------------------------------------------- */
void app_pointer_down(int x, int y)
{
    uint32_t now_ms = hmi_timer_get_ms();
    hmi_security_register_activity(&g_security, now_ms);
    hmi_gesture_begin(&g_gesture, x, y, now_ms);
}

void app_pointer_move(int x, int y)
{
    if (g_gesture.active) hmi_gesture_move(&g_gesture, x, y);
}

void app_pointer_up(int x, int y)
{
    uint32_t now_ms = hmi_timer_get_ms();
    int32_t tx = 0, ty = 0;
    hmi_gesture_t g = hmi_gesture_end(&g_gesture, x, y, now_ms, g_renderer.screen_w, &tx, &ty);
    hmi_security_register_activity(&g_security, now_ms);
    if (g == GESTURE_TAP && g_emergency && g_modal == HMI_MODAL_NONE) {
        hmi_hit_t hit = hmi_layout_emergency_hit_test(g_renderer.screen_w, g_renderer.screen_h, tx, ty);
        ui_dispatch_hit(&hit, now_ms);
    } else if (g_emergency && g_modal == HMI_MODAL_NONE) {
        /* no swipes or other gestures on the red screen */
    } else if (g == GESTURE_SWIPE_LEFT && g_modal == HMI_MODAL_NONE) {
        ui_navigate(1);
    } else if (g == GESTURE_SWIPE_RIGHT && g_modal == HMI_MODAL_NONE) {
        ui_navigate(-1);
    } else if (g == GESTURE_TAP && ui_alarm_row_tap(tx, ty, now_ms)) {
        /* handled: a per-alarm Ack button */
    } else if (g == GESTURE_TAP) {
        uint8_t ntok = 0;
        const uint32_t *tok = ui_active_tokens(&ntok);
        hmi_hit_t hit = hmi_layout_hit_test(g_renderer.screen_w, g_renderer.screen_h, g_active_screen,
                                            ui_banner_active(), g_modal, tok, ntok,
                                            g_dev_view, g_ndev, g_dev_page, g_fp_slot, tx, ty);
        /* Device view: the content area belongs to the one device shown */
        const hmi_device_info_t *fd = ui_dev(g_focus_slot);
        if (fd && g_active_screen == SCREEN_PROCESS_OVERVIEW && g_modal == HMI_MODAL_NONE &&
            hmi_rect_contains(hmi_layout_content(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active()), tx, ty))
            hit = hmi_layout_focus_hit_test(g_renderer.screen_w, g_renderer.screen_h, ui_banner_active(), fd, tx, ty);
        ui_dispatch_hit(&hit, now_ms);
    }
    hal_request_repaint();
}

void app_pointer_cancel(void)
{
    hmi_gesture_cancel(&g_gesture);
}

void app_key(uint16_t key, bool shift)
{
    uint32_t now_ms = hmi_timer_get_ms();
    hmi_security_register_activity(&g_security, now_ms);
    ui_handle_key(key, shift, now_ms);
    hal_request_repaint();
}

void app_resize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    hmi_renderer_resize(&g_renderer, w, h);
    hal_request_repaint();
}

bool app_running(void)
{
    return hal_atomic_load(&g_running) != 0;
}

void app_stop(void)
{
    hal_atomic_store(&g_running, 0);
}

uint32_t app_tick(void)
{
    uint32_t now_ms = hmi_timer_get_ms();
    if ((uint32_t)(now_ms - g_ui_last_frame_ms) >= HMI_UI_FRAME_PERIOD_MS) app_frame(now_ms);
    uint32_t elapsed = (uint32_t)(hmi_timer_get_ms() - g_ui_last_frame_ms);
    return elapsed >= HMI_UI_FRAME_PERIOD_MS ? 0u : (uint32_t)(HMI_UI_FRAME_PERIOD_MS - elapsed);
}

/* ------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* ------------------------------------------------------------------------- */
static void exe_dir_path(char *out, size_t cap, const char *file)
{
    char dir[HMI_PATH_MAX];
    hal_exe_dir(dir, sizeof(dir));
    snprintf(out, cap, "%.200s%.50s", dir, file);
}

/* Returns -1 to continue, otherwise a process exit code. */
/* --- Device key (hmi.key) --------------------------------------------------------- */
/* Loads (or creates) the key and gives it to the audit log and the PIN store. Returns the status;
 * on anything but LOADED / CREATED both run with an all-zero key and say so. */
static hmi_key_status_t device_key_setup(bool create, hmi_security_ctx_t *sec)
{
    hmi_key_status_t st = hmi_device_key_load(g_key_path, create, g_device_key);
    bool ok = st == HMI_KEY_LOADED || st == HMI_KEY_CREATED;
    uint8_t k[HMI_DEVICE_KEY_LEN];
    if (ok) {                                   /* one key per purpose (HKDF), never the master itself */
        hmi_device_key_derive(g_device_key, HMI_KEY_PURPOSE_AUDIT_LOG, k);
        hmi_alarm_log_set_key(k);
        hmi_device_key_derive(g_device_key, HMI_KEY_PURPOSE_CREDENTIALS, k);
        if (sec) hmi_security_set_device_key(sec, k);
        hal_secure_zero(k, sizeof(k));
    } else {
        hmi_alarm_log_set_key(NULL);
        if (sec) hmi_security_set_device_key(sec, NULL);
    }
    hal_secure_zero(g_device_key, sizeof(g_device_key));   /* the master is not kept in memory */
    return st;
}

/* --- Credential vault (security.vault): signed with the device key ------------------ */
#define VAULT_MISSING 0
#define VAULT_LOADED  1
/* VAULT_LOADED, VAULT_MISSING, or -(hmi_vault_result_t) when present but not accepted */
static int vault_load(hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE + 1];
    hal_file_t *f = hal_file_open(g_vault_path, HAL_FILE_READ);
    if (!f) return VAULT_MISSING;
    int got = hal_file_read(f, buf, sizeof(buf));
    hal_file_close(f);
    hmi_vault_result_t r = got > 0 ? hmi_security_vault_import(sec, buf, (size_t)got) : HMI_VAULT_DAMAGED;
    hal_secure_zero(buf, sizeof(buf));
    return r == HMI_VAULT_OK ? VAULT_LOADED : -(int)r;
}

static const char *vault_problem(int v)
{
    switch (-v) {
        case HMI_VAULT_LEGACY:      return "Old unsigned vault: set PINs again";
        case HMI_VAULT_AUTH_FAILED: return "Vault signature invalid: factory PINs";
        default:                    return "Credential vault damaged: factory PINs";
    }
}

static bool vault_save(const hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE];
    char tmp[HMI_PATH_MAX + 8];
    if (hmi_security_vault_export(sec, buf, sizeof(buf)) != HMI_VAULT_SIZE) return false;
    snprintf(tmp, sizeof(tmp), "%.255s.tmp", g_vault_path);
    hal_file_t *f = hal_file_open(tmp, HAL_FILE_WRITE_NEW);
    if (!f) return false;
    if (!hal_file_restrict(tmp, g_key_path)) {        /* the vault follows the key's owner and rights */
        hal_file_close(f);
        hal_file_remove(tmp);
        return false;
    }
    bool ok = hal_file_write(f, buf, sizeof(buf)) && hal_file_sync(f);
    hal_file_close(f);
    /* atomic replace: a power cut leaves either the old or the new vault, never half of one */
    if (!ok || !hal_file_replace(tmp, g_vault_path)) {
        hal_file_remove(tmp);
        return false;
    }
    return true;
}

static bool ieq(const char *a, const char *b)       /* ASCII case-insensitive equality */
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return *a == *b;
}

static int parse_role(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "1") == 0 || ieq(s, "operator")) return ROLE_OPERATOR;
    if (strcmp(s, "2") == 0 || ieq(s, "maintenance")) return ROLE_MAINTENANCE;
    if (strcmp(s, "3") == 0 || ieq(s, "engineer")) return ROLE_ENGINEER;
    return -1;
}

static int run_set_pin(void)
{
    static hmi_security_ctx_t sec;
    char out[260];
    /* A running HMI keeps the PINs it loaded at start: changing them under it would leave the old
     * ones (or bench mode) in force until a restart. Same lock as the HMI itself. */
    char lock[HMI_PATH_MAX + 8];
    snprintf(lock, sizeof(lock), "%s.lock", g_key_path);
    if (!hal_instance_lock(lock)) {
        hal_console_print("The HMI is running: close it first, then set the PIN.\r\n");
        return 4;
    }
    hmi_security_init(&sec);
    hmi_key_status_t ks = device_key_setup(true, &sec);
    if (ks != HMI_KEY_LOADED && ks != HMI_KEY_CREATED) {
        snprintf(out, sizeof(out), "device key %.150s: %s - PINs cannot be stored safely\r\n", g_key_path,
                 hmi_device_key_status_name(ks));
        hal_console_print(out);
        return 3;
    }
    int v = vault_load(&sec);
    if (v < 0) {
        snprintf(out, sizeof(out), "%s: the other roles keep their factory PINs until they are set\r\n", vault_problem(v));
        hal_console_print(out);
        hmi_security_load_default_pins(&sec);
    }

    static char pin[32], again[32];
    snprintf(out, sizeof(out), "New PIN for %s (6-8 digits): ", hmi_security_role_name((hmi_role_t)g_set_pin_role));
    bool ok = hal_console_read_secret(out, pin, sizeof(pin)) &&
              hal_console_read_secret("Repeat the PIN: ", again, sizeof(again));
    if (!ok) {
        hal_console_print("no PIN entered (needs an interactive console, or the PIN twice on stdin)\r\n");
        hal_secure_zero(pin, sizeof(pin));
        hal_secure_zero(again, sizeof(again));
        return 64;
    }
    bool same = strcmp(pin, again) == 0;
    hal_secure_zero(again, sizeof(again));
    if (!same) {
        hal_secure_zero(pin, sizeof(pin));
        hal_console_print("the two PINs differ: nothing changed\r\n");
        return 64;
    }
    uint8_t salt[8];
    if (!hal_random(salt, sizeof(salt))) {
        hal_secure_zero(pin, sizeof(pin));
        hal_console_print("no OS random source available\r\n");
        return 3;
    }
    hmi_pin_set_result_t r = hmi_security_set_pin(&sec, (hmi_role_t)g_set_pin_role, pin, salt);
    hal_secure_zero(pin, sizeof(pin));
    if (r != HMI_PIN_SET_OK) {
        snprintf(out, sizeof(out), "PIN not accepted: %s\r\n", hmi_security_pin_set_message(r));
        hal_console_print(out);
        return 64;
    }
    if (!vault_save(&sec)) {
        snprintf(out, sizeof(out), "could not write %.150s\r\n", g_vault_path);
        hal_console_print(out);
        return 3;
    }
    snprintf(out, sizeof(out), "PIN for %s updated in %.150s\r\n", hmi_security_role_name((hmi_role_t)g_set_pin_role), g_vault_path);
    hal_console_print(out);
    if (sec.default_pins) {
        hal_console_print("Still on factory PINs:");
        for (int role = ROLE_OPERATOR; role <= ROLE_ENGINEER; role++)
            if (sec.factory[role]) { hal_console_print(" "); hal_console_print(hmi_security_role_name((hmi_role_t)role)); }
        hal_console_print(" - those roles cannot sign in until their PIN is set.\r\n");
    }
    return 0;
}

static int run_verify_log(void)
{
    char out[360];
    hmi_key_status_t ks = device_key_setup(false, NULL);
    if (ks != HMI_KEY_LOADED) {
        snprintf(out, sizeof(out), "device key %.150s: %s - only unsigned logs can be checked\r\n", g_key_path,
                 hmi_device_key_status_name(ks));
        hal_console_print(out);
    }
    uint32_t lines = 0, bad = 0, first_bad = 0;
    bool ok = hmi_alarm_log_verify_file(g_verify_path, &lines, &bad, &first_bad);
    if (ok) {
        snprintf(out, sizeof(out), "%.200s: %lu lines, chain intact -> VERIFIED\r\n  (%s)\r\n",
                 g_verify_path, (unsigned long)lines, hmi_alarm_log_verify_note());
    } else if (lines == 0) {
        snprintf(out, sizeof(out), "%.200s: cannot read file or file is empty -> FAILED\r\n", g_verify_path);
    } else {
        snprintf(out, sizeof(out), "%.200s: %lu lines, %lu fail, first at line %lu -> FAILED\r\n  (%s)\r\n",
                 g_verify_path, (unsigned long)lines, (unsigned long)bad, (unsigned long)first_bad,
                 hmi_alarm_log_verify_note());
    }
    hal_console_print(out);
    return ok ? 0 : 2;
}

/* --- Site configuration file: command-line options, one or more per line ------------- */
#define CFG_MAX_BYTES  4096
#define CFG_MAX_TOKENS 96
static char  s_cfg_text[CFG_MAX_BYTES + 1];
static char *s_cfg_argv[CFG_MAX_TOKENS + 1];

/* Split the text into tokens: whitespace separated, "double quotes" keep spaces, # to the end
 * of the line is a comment. Returns the token count (argv[0] is a placeholder), -1 = too many. */
static int cfg_tokenize(char *t, char **argv, int cap)
{
    int n = 1;
    argv[0] = s_cfg_text;
    while (*t) {
        while (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n') t++;
        if (!*t) break;
        if (*t == '#') { while (*t && *t != '\n') t++; continue; }
        if (n >= cap) return -1;
        if (*t == '"') {
            argv[n++] = ++t;
            while (*t && *t != '"' && *t != '\n') t++;
        } else {
            argv[n++] = t;
            while (*t && *t != ' ' && *t != '\t' && *t != '\r' && *t != '\n') t++;
        }
        if (*t) *t++ = '\0';
    }
    return n;
}

/* 0 = no file, > 0 = token count, < 0 = error (message printed) */
static int cfg_load(const char *path, bool required)
{
    hal_file_t *f = hal_file_open(path, HAL_FILE_READ);
    char out[260];
    if (!f) {
        if (!required) return 0;
        snprintf(out, sizeof(out), "cannot read config file %.200s\r\n", path);
        hal_console_print(out);
        return -1;
    }
    size_t len = 0;
    for (;;) {
        int r = hal_file_read(f, s_cfg_text + len, CFG_MAX_BYTES - len);
        if (r <= 0) break;
        len += (size_t)r;
        if (len == CFG_MAX_BYTES) break;
    }
    char extra;
    bool too_big = len == CFG_MAX_BYTES && hal_file_read(f, &extra, 1) > 0;
    hal_file_close(f);
    s_cfg_text[len] = '\0';
    int n = too_big ? -1 : cfg_tokenize(s_cfg_text, s_cfg_argv, CFG_MAX_TOKENS);
    if (n < 0) {
        snprintf(out, sizeof(out), "config file %.200s is too large (4 KB, 95 options at most)\r\n", path);
        hal_console_print(out);
        return -1;
    }
    return n;
}

static int parse_options(int argc, char **argv);

/* --- Secrets: the device key and the credential vault live in the protected system folder ---
 * (hal_secure_dir). A key or vault left next to the program by an older installation is moved
 * there once, so a commissioned panel keeps its key, its PINs and its audit chain. */
static char g_default_key[HMI_PATH_MAX], g_default_vault[HMI_PATH_MAX];
static bool g_secret_moved, g_secure_dir_refused;

static bool file_exists(const char *path)
{
    hal_file_t *f = hal_file_open(path, HAL_FILE_READ);
    if (!f) return false;
    hal_file_close(f);
    return true;
}

/* Copy (not rename: the folders may be on different drives), protect, then delete the old file */
static bool move_secret(const char *from, const char *to)
{
    static uint8_t buf[4096];
    hal_file_t *f = hal_file_open(from, HAL_FILE_READ);
    if (!f) return false;
    int n = hal_file_read(f, buf, sizeof(buf));
    hal_file_close(f);
    bool ok = false;
    if (n > 0 && n < (int)sizeof(buf)) {
        hal_file_t *g = hal_file_open(to, HAL_FILE_WRITE_NEW);
        if (g) {
            ok = hal_file_restrict(to, NULL) && hal_file_write(g, buf, (size_t)n) && hal_file_sync(g);
            hal_file_close(g);
            if (!ok) hal_file_remove(to);
        }
    }
    hal_secure_zero(buf, sizeof(buf));
    return ok && hal_file_remove(from);
}

/* Works out the default places only; the folder is made in secret_migrate(), once we know the
 * defaults are used (not --key / --vault). */
static void secret_default_paths(void)
{
    char dir[HMI_PATH_MAX];
    if (hal_secure_dir(dir, sizeof(dir), false)) {
        snprintf(g_default_key, sizeof(g_default_key), "%.240shmi.key", dir);
        snprintf(g_default_vault, sizeof(g_default_vault), "%.240ssecurity.vault", dir);
    } else {                                              /* no protected folder here */
        exe_dir_path(g_default_key, sizeof(g_default_key), "hmi.key");
        exe_dir_path(g_default_vault, sizeof(g_default_vault), "security.vault");
    }
    copy_str(g_key_path, sizeof(g_key_path), g_default_key);
    copy_str(g_vault_path, sizeof(g_vault_path), g_default_vault);
}

/* Only when the default places are in use (not --key / --vault) */
static void secret_migrate(void)
{
    static const char * const names[2] = { "hmi.key", "security.vault" };
    const char *dst[2] = { g_default_key, g_default_vault };
    const char *used[2] = { g_key_path, g_vault_path };
    bool key_default = strcmp(g_key_path, g_default_key) == 0, vault_default = strcmp(g_vault_path, g_default_vault) == 0;
    if (!key_default && !vault_default) return;
    char dir[HMI_PATH_MAX];
    if (!hal_secure_dir(dir, sizeof(dir), true)) {        /* not usable here: next to the program */
        g_secure_dir_refused = true;
        if (key_default) exe_dir_path(g_key_path, sizeof(g_key_path), "hmi.key");
        if (vault_default) exe_dir_path(g_vault_path, sizeof(g_vault_path), "security.vault");
        return;
    }
    for (int i = 0; i < 2; i++) {
        char old[HMI_PATH_MAX];
        exe_dir_path(old, sizeof(old), names[i]);
        if (strcmp(used[i], dst[i]) != 0 || strcmp(old, dst[i]) == 0) continue;
        if (file_exists(dst[i]) || !file_exists(old)) continue;
        if (move_secret(old, dst[i])) g_secret_moved = true;
    }
}

/* Sound files: emergency.mp3 / alarm.mp3 next to the program when present, else the built-in tone */
static char g_emergency_sound[HMI_PATH_MAX], g_alarm_sound[HMI_PATH_MAX];

static void sound_defaults(void)
{
    exe_dir_path(g_emergency_sound, sizeof(g_emergency_sound), "emergency.mp3");
    if (!file_exists(g_emergency_sound)) g_emergency_sound[0] = '\0';
    exe_dir_path(g_alarm_sound, sizeof(g_alarm_sound), "alarm.mp3");
    if (!file_exists(g_alarm_sound)) g_alarm_sound[0] = '\0';
    /* no emergency.mp3: the built-in EMERGENCY sweep, never the alarm sound - an operator must
     * hear the difference between "an alarm" and "EMERGENCY was pressed" */
}

int app_parse_args(int argc, char **argv)
{
    exe_dir_path(g_log_path, sizeof(g_log_path), HMI_ALARM_LOG_PATH);
    secret_default_paths();
    sound_defaults();

    /* hmi.cfg (or --config) first, so the command line can add to it */
    char cfg[HMI_PATH_MAX];
    bool use_cfg = true, required = false;
    exe_dir_path(cfg, sizeof(cfg), "hmi.cfg");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-config") == 0) use_cfg = false;
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) { copy_str(cfg, sizeof(cfg), argv[i + 1]); required = true; }
    }
    if (use_cfg) {
        int n = cfg_load(cfg, required);
        if (n < 0) return 64;
        if (n > 1) {
            int rc = parse_options(n, s_cfg_argv);
            if (rc >= 0) return rc;
            const char *base = strrchr(cfg, '/');
            const char *b2 = strrchr(cfg, '\\');
            if (!base || (b2 && b2 > base)) base = b2;
            snprintf(g_cfg_note, sizeof(g_cfg_note), "Site config %.30s: %d options", base ? base + 1 : cfg, n - 1);
        }
    }
    int rc = parse_options(argc, argv);
    if (rc >= 0) return rc;
    hal_sound_file(HAL_SOUND_EMERGENCY, g_emergency_sound);
    hal_sound_file(HAL_SOUND_ALARM, g_alarm_sound);
    secret_migrate();
    if (g_verify_path[0]) return run_verify_log();
    if (g_set_pin_role >= 0) return run_set_pin();
    return -1;
}

static int parse_options(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--plc") == 0 && next) {
            if (!hmi_discovery_add(next, true)) {
                char out[160];
                snprintf(out, sizeof(out), "bad --plc address: %.100s (use ip[:port][/uN][@module])\r\n", next);
                hal_console_print(out);
                return 64;
            }
            g_have_plc_arg = true;
            i++;
        } else if (strcmp(a, "--scan") == 0 && next) {
            if (!hmi_discovery_add(next, false)) {
                char out[200];
                snprintf(out, sizeof(out), "bad --scan range: %.100s (e.g. 192.168.1.0/24 or 10.0.0.5:502-510; "
                                           "at most %d addresses in total)\r\n", next, HMI_DISCOVERY_MAX_TARGETS);
                hal_console_print(out);
                return 64;
            }
            i++;
        } else if (strcmp(a, "--config") == 0 && next) {
            i++;                                            /* already handled before the other options */
        } else if (strcmp(a, "--no-config") == 0) {
        } else if (strcmp(a, "--no-discovery") == 0) {
            g_discovery_on = false;
        } else if (strcmp(a, "--remove-after-ms") == 0 && next) {
            g_remove_after_ms = (uint32_t)strtoul(next, NULL, 10); i++;
        } else if (strcmp(a, "--max-devices") == 0 && next) {
            long n = strtol(next, NULL, 10);
            if (n < 1 || n > HMI_MAX_DEVICES) {
                char out[96];
                snprintf(out, sizeof(out), "--max-devices: give 1..%d (the most this build holds)\r\n", HMI_MAX_DEVICES);
                hal_console_print(out);
                return 64;
            }
            hmi_devmgr_set_limit((int)n); i++;
        } else if (strcmp(a, "--log") == 0 && next) {
            copy_str(g_log_path, sizeof(g_log_path), next); i++;
        } else if (strcmp(a, "--no-log") == 0) {
            g_log_enabled = false;
        } else if (strcmp(a, "--exit-after-ms") == 0 && next) {
            g_exit_after_ms = (uint32_t)strtoul(next, NULL, 10); i++;
        } else if (strcmp(a, "--report") == 0 && next) {
            copy_str(g_report_path, sizeof(g_report_path), next); i++;
        } else if (strcmp(a, "--vault") == 0 && next) {
            copy_str(g_vault_path, sizeof(g_vault_path), next); i++;
        } else if (strcmp(a, "--key") == 0 && next) {
            copy_str(g_key_path, sizeof(g_key_path), next); i++;
        } else if (strcmp(a, "--allow-factory-pins") == 0) {
            g_allow_factory_pins = true;
        } else if (strcmp(a, "--no-sound") == 0) {
            hal_sound_enable(false);
        } else if (strcmp(a, "--emergency-sound") == 0 && next) {
            copy_str(g_emergency_sound, sizeof(g_emergency_sound), next); i++;
        } else if (strcmp(a, "--alarm-sound") == 0 && next) {
            copy_str(g_alarm_sound, sizeof(g_alarm_sound), next); i++;
        } else if (strcmp(a, "--color-scheme") == 0 && next) {
            if (strcmp(next, "high-performance") == 0) g_high_performance = true;
            else if (strcmp(next, "status") == 0) g_high_performance = false;
            else {
                hal_console_print("usage: --color-scheme status|high-performance\r\n");
                return 64;
            }
            i++;
        } else if (strcmp(a, "--set-pin") == 0 && next) {
            g_set_pin_role = parse_role(next);
            if (g_set_pin_role < 0) {
                hal_console_print("usage: --set-pin <operator|maintenance|engineer>   (the PIN is asked for)\r\n");
                return 64;
            }
            i++;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                /* A PIN on the command line ends up in shell history and the process list. */
                hal_console_print("PINs are not accepted on the command line: run --set-pin <role> and type it "
                                  "when asked\r\n");
                return 64;
            }
        } else if (strcmp(a, "--verify-log") == 0 && next) {
            copy_str(g_verify_path, sizeof(g_verify_path), next); i++;
        } else {
            char out[160];
            snprintf(out, sizeof(out), "unknown or incomplete argument: %s\r\n", a);
            hal_console_print(out);
            return 64;
        }
    }
    return -1;
}

static void write_report(void)
{
    if (!g_report_path[0]) return;
    hal_file_t *h = hal_file_open(g_report_path, HAL_FILE_WRITE_NEW);
    if (!h) return;
    hmi_alarm_log_stats_t ls;
    hmi_alarm_log_get_stats(&ls);
    const hmi_fieldbus_stats_t *fb = &g_snap.fb_stats;
    char buf[1800];
    int n;
    for (int d = 0; d < g_snap.ndev; d++) {         /* one line per present device */
        const hmi_device_info_t *dv = &g_snap.devices[d];
        if (!hmi_device_present(dv)) continue;
        n = snprintf(buf, sizeof(buf), "device%d=%s|%s|%s|state%u|ok%lu|to%lu\r\n", d,
                     dv->label, dv->endpoint, dv->profile->key, (unsigned)dv->state,
                     (unsigned long)dv->responses, (unsigned long)dv->timeouts);
        if (n > 0) hal_file_write(h, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
    n = snprintf(buf, sizeof(buf),
        "latency_samples=%lu\r\nlatency_avg_us=%lu\r\nlatency_max_us=%lu\r\nlatency_over_target=%lu\r\n"
        "latency_target_ms=%u\r\nalarm_rate_10min=%u\r\nalarm_rate_peak_10min=%u\r\nalarm_floods=%lu\r\n"
        "alarm_shelved=%u\r\nalarm_chattering=%u\r\n",
        (unsigned long)g_latency.total_n,
        (unsigned long)(g_latency.total_n ? g_latency.total_sum_us / g_latency.total_n : 0u),
        (unsigned long)g_latency.total_max_us, (unsigned long)g_latency.over_target,
        (unsigned)HMI_LATENCY_TARGET_MS, (unsigned)g_snap.alarms.rate_10min,
        (unsigned)g_snap.alarms.rate_peak_10min, (unsigned long)g_snap.alarms.flood_count,
        (unsigned)g_snap.alarms.shelved_count, (unsigned)g_snap.alarms.chatter_count);
    if (n > 0) hal_file_write(h, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    n = snprintf(buf, sizeof(buf),
        "fieldbus_loop_hz=%lu\r\nfieldbus_period_avg_us=%lu\r\nfieldbus_period_min_us=%lu\r\n"
        "fieldbus_period_max_us=%lu\r\nfieldbus_late_count=%lu\r\nfieldbus_loop_count=%lu\r\n"
        "fieldbus_high_res_timer=%d\r\npacer_overruns=%lu\r\nui_frame_hz=%lu\r\nui_max_frame_ms=%lu\r\n"
        "seqlock_retries=%lu\r\ncrc_mismatches=%lu\r\nui_watchdog_faults=%lu\r\n"
        "ui_wd_fieldbus_alive=%d\r\nfb_wd_ui_alive=%d\r\nfb_wd_fault_count=%lu\r\n"
        "devices_present=%u\r\n"
        "alarm_latch=%d\r\nalarm_highest=%d\r\nalarm_active=%u\r\nalarm_unacked=%u\r\njournal_total=%lu\r\n"
        "log_open=%d\r\nlog_records_written=%lu\r\nlog_records_lost=%lu\r\nlog_write_errors=%lu\r\n"
        "role=%s\r\naccess_denied=%lu\r\nexe_size=%lu\r\nbuild=%s\r\n",
        (unsigned long)fb->loop_hz, (unsigned long)fb->period_avg_us, (unsigned long)fb->period_min_us,
        (unsigned long)fb->period_max_us, (unsigned long)fb->late_count, (unsigned long)fb->loop_count,
        fb->high_res_timer ? 1 : 0, (unsigned long)fb->pacer_overruns, (unsigned long)g_ui_frame_hz,
        (unsigned long)g_ui_max_frame_ms, (unsigned long)g_seqlock_retries, (unsigned long)g_crc_mismatches,
        (unsigned long)g_ui_wd_faults_seen, g_ui_wd.fieldbus_alive ? 1 : 0, g_snap.fb_watchdog.ui_alive ? 1 : 0,
        (unsigned long)g_snap.fb_watchdog.fault_count,
        (unsigned)g_ndev,
        (int)g_snap.alarms.latch_state, (int)g_snap.alarms.highest_severity,
        (unsigned)g_snap.alarms.active_count, (unsigned)g_snap.alarms.unacked_count,
        (unsigned long)g_snap.alarms.journal_total,
        ls.open ? 1 : 0, (unsigned long)ls.records_written, (unsigned long)ls.records_lost,
        (unsigned long)ls.write_errors,
        hmi_security_role_name(g_security.current_role), (unsigned long)g_security.denied_count,
        (unsigned long)g_exe_size, g_hmi_build_info);
    if (n > 0) hal_file_write(h, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    hal_file_close(h);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */
int app_init(void)
{
    /* Before anything is written: a second HMI on the same files would break the log chain */
    char lock[HMI_PATH_MAX + 8];
    snprintf(lock, sizeof(lock), "%s.lock", g_key_path);
    if (!hal_instance_lock(lock)) return 4;

    hmi_timer_init();
    uint32_t now_ms = hmi_timer_get_ms();

    hmi_security_init(&g_security);                     /* starts as VIEWER with factory PINs */
    hmi_security_register_activity(&g_security, now_ms);
    hmi_key_status_t ks = device_key_setup(true, &g_security);
    if (g_secret_moved) ui_post_event(ALARM_SEV_INFO, "Key and PINs moved to protected folder");
    if (g_secure_dir_refused) ui_post_event(ALARM_SEV_WARNING, "Protected folder unsafe: key kept here");
    if (ks == HMI_KEY_CREATED) {
        ui_post_event(ALARM_SEV_INFO, "Device key created: audit log signed");
    } else if (ks != HMI_KEY_LOADED) {
        ui_post_event(ALARM_SEV_CRITICAL, "Device key unusable: log NOT signed");
        ui_toast("Device key unusable - audit log is not signed", now_ms);
    }
    int vault = vault_load(&g_security);
    if (vault < 0) {
        hmi_security_load_default_pins(&g_security);
        ui_post_event(ALARM_SEV_WARNING, vault_problem(vault));
        ui_toast(vault_problem(vault), now_ms);
    }
    /* A commissioned panel (a signed vault with at least one site PIN) never goes back to bench
     * mode, whatever hmi.cfg says: editing the config file must not re-open the factory PINs. */
    bool commissioned = false;
    if (vault == VAULT_LOADED)
        for (int role = ROLE_OPERATOR; role <= ROLE_ENGINEER; role++) if (!g_security.factory[role]) commissioned = true;
    if (g_allow_factory_pins && commissioned) {
        g_allow_factory_pins = false;
        ui_post_event(ALARM_SEV_WARNING, "Bench mode refused: site PINs exist");
    }
    g_security.allow_factory_pins = g_allow_factory_pins;
    if (g_allow_factory_pins && g_security.default_pins)
        ui_post_event(ALARM_SEV_WARNING, "BENCH MODE: factory PINs allowed");
    else if (g_security.default_pins)
        ui_post_event(ALARM_SEV_WARNING, "Factory PINs: those roles locked out");
    if (g_cfg_note[0]) ui_post_event(ALARM_SEV_INFO, g_cfg_note);
    hmi_seqlock_init(&g_seqlock);

    /* Drivers and tag metadata: registered once, before any worker thread exists. */
    /* Device tag blocks are bound at runtime by the device manager as PLCs come and go. */
    g_host_driver   = hmi_driver_host_get();
    hmi_tag_registry_reset();
    bool meta_ok = true;
    if (g_host_driver) {
        meta_ok &= hmi_tag_register_meta(g_host_driver->tag_metadata, g_host_driver->tag_count);
        if (g_host_driver->init) g_host_driver->init();
    }
    meta_ok &= hmi_tag_register_meta(s_system_meta, (uint8_t)(sizeof(s_system_meta) / sizeof(s_system_meta[0])));
    if (!meta_ok) {
        hal_fatal("Tag metadata conflict between drivers (duplicate tag id).");
        hmi_timer_shutdown();
        return 3;
    }

    hmi_watchdog_init(&g_ui_wd, now_ms);
    hal_atomic_store(&g_hb_ui_ms, (int32_t)now_ms);
    hal_atomic_store(&g_hb_fieldbus_ms, (int32_t)now_ms);
    g_exe_size = hal_exe_size();

    hmi_renderer_init(&g_renderer, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT);
    hmi_renderer_set_scheme(&g_renderer, g_high_performance);

    if (g_log_enabled && !hmi_alarm_log_open(g_log_path, HMI_ALARM_LOG_MAX_BYTES)) {
        ui_toast("Alarm log file could not be opened", now_ms);
    }
    return 0;
}

bool app_start_workers(void)
{
    if (!hal_thread_start(&g_fb_thread, fieldbus_thread, NULL, HAL_PRIO_HIGH, HMI_STACK_FIELDBUS)) {
        hal_fatal("Could not start the fieldbus thread.");
        return false;
    }

    /* Discovery: localhost demo ports unless the user configured PLC addresses */
    if (g_discovery_on) {
        if (!g_have_plc_arg) hmi_discovery_add_defaults();
        hal_thread_start(&g_disc_thread, discovery_thread, NULL, HAL_PRIO_LOW, HMI_STACK_DISCOVERY);
    } else if (g_have_plc_arg) {
        hal_thread_start(&g_disc_thread, discovery_thread, NULL, HAL_PRIO_NORMAL, HMI_STACK_DISCOVERY);  /* probes only the --plc addresses */
    }

    hal_trim_memory();

    g_ui_start_ms = hmi_timer_get_ms();
    g_ui_last_frame_ms = g_ui_start_ms - HMI_UI_FRAME_PERIOD_MS;
    g_ui_last_beep_ms = g_ui_start_ms;
    g_ui_last_trim_ms = g_ui_start_ms;
    g_ui_fps_window_ms = g_ui_start_ms;
    g_ui_fps_frames = 0;
    return true;
}

void app_shutdown(void)
{
    hal_atomic_store(&g_running, 0);
    hal_thread_join(&g_fb_thread, 3000);
    hal_thread_join(&g_disc_thread, 3000);

    /* Flush the journal events produced during shutdown, then close the log. */
    ui_take_snapshot();
    if (g_have_snapshot) hmi_alarm_log_sync(&g_snap.alarms);
    hmi_alarm_log_close();
    write_report();
    hmi_timer_shutdown();
}
