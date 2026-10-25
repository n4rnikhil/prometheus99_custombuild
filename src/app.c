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
 * Command line
 *   --plc <ip[:port][/unit]>   a PLC address to watch (attached even if it cannot identify itself)
 *   --scan <spec>              extra discovery range, e.g. 192.168.1.0/24 or 10.0.0.5:502-510
 *   --no-discovery             only the --plc addresses / simulator, no scanning
 *   --sim [type]               add a built-in simulated device: tank (default), drive, conveyor, boiler,
 *                              pumps, compressor, ahu, abb (repeatable)
 *   --remove-after-ms <n>      offline devices leave the dashboard after n ms (0 = never)
 *   --log <path>               alarm log file (default alarms.log next to the executable)
 *   --no-log                   disable the persistent alarm log
 *   --exit-after-ms <n>        close automatically after n ms (tests / measure.bat)
 *   --report <path>            write measured runtime statistics on exit
 *   --verify-log <path>        verify an alarm log's CRCs, print result, exit (0 = ok)
 *   --set-pin <role> <pin>     change a role PIN in the credential vault, exit
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../include/app.h"
#include "../include/config.h"
#include "../include/core/seqlock.h"
#include "../include/core/tags.h"
#include "../include/core/alarms.h"
#include "../include/core/alarm_log.h"
#include "../include/core/watchdog.h"
#include "../include/core/security.h"
#include "../include/core/crc32.h"
#include "../include/core/runtime_stats.h"
#include "../include/renderer/micro_renderer.h"
#include "../include/renderer/layout.h"
#include "../include/drivers/device_manager.h"
#include "../include/drivers/discovery.h"
#include "../include/drivers/driver_host.h"
#include "../include/drivers/plc_sim.h"
#include "../include/hal/atomic.h"
#include "../include/hal/fs.h"
#include "../include/hal/gesture.h"
#include "../include/hal/platform.h"
#include "../include/hal/sys.h"
#include "../include/hal/thread.h"
#include "../include/hal/timer.h"

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
} hmi_shared_state_t;

static hmi_shared_state_t g_shared;       /* written only by the fieldbus thread */
static hmi_seqlock_t      g_seqlock;
static hmi_shared_state_t g_snap;         /* UI thread's private copy */

/* Heartbeats (uptime ms), each written by one thread, read by the other */
static hal_atomic_t g_hb_ui_ms;
static hal_atomic_t g_hb_fieldbus_ms;
static hal_atomic_t g_running = 1;

/* UI -> fieldbus command bits */
#define CMD_BIT_ACK          (1 << 0)
#define CMD_BIT_SILENCE      (1 << 1)
#define CMD_BIT_ESTOP        (1 << 2)
#define CMD_BIT_ESTOP_RESET  (1 << 3)
#define CMD_BIT_INJECT_FAULT (1 << 6)
#define CMD_BIT_WD_RESET     (1 << 7)
static hal_atomic_t g_cmd_bits;
/* UI -> fieldbus device commands (SPSC ring: UI produces, fieldbus consumes) */
#define DEV_CMD_RING 16
typedef struct { uint8_t slot, action; int16_t value; } dev_cmd_t;
static dev_cmd_t     g_dev_cmds[DEV_CMD_RING];
static hal_atomic_t  g_dev_cmd_head;   /* written by UI */
static hal_atomic_t  g_dev_cmd_tail;   /* written by fieldbus */

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
static const hmi_tag_meta_t s_system_meta[] = {
    { SYS_TAG_WATCHDOG, TAG_TYPE_DIGITAL, "HMI watchdog failover", "", 0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "A control loop stalled; check the HMI host for overload", "Maintenance: reset the failover latch in Settings", NULL },
};
static uint8_t  g_sim_types[HMI_MAX_DEVICES];   /* --sim devices (sim_type_t) */
static uint8_t  g_nsim;
static bool     g_discovery_on = true;
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
static int                   g_set_pin_role = -1;
static char                  g_set_pin_value[HMI_PIN_MAX_LEN + 1];
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
                             "E-stop commanded by operator");
        if (hmi_devmgr_estop(true, tags, alarms, now_ms) == 0) {
            hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_CRITICAL, ALARM_PT_NORMAL,
                                 "No device took the E-stop: use hardwired");
        }
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
                                 "E-stop reset: nothing to reset");
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
    do {
        seq = hmi_seqlock_read_begin(&g_seqlock);
        memcpy(devs, g_shared.devices, sizeof(devs));
    } while (hmi_seqlock_read_retry(&g_seqlock, seq));
    char ep[24];
    snprintf(ep, sizeof(ep), "%s:%u", host, (unsigned)port);
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
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
    hmi_devmgr_publish(devs, now_ms);
    hmi_seqlock_write_begin(&g_seqlock);
    g_shared.tags        = *tags;
    g_shared.alarms      = *alarms;
    g_shared.fb_watchdog = *wd;
    g_shared.fb_stats    = *st;
    memcpy(g_shared.devices, devs, sizeof(devs));
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
    for (uint8_t i = 0; i < g_nsim; i++) hmi_devmgr_attach_simulator(g_sim_types[i], &tags, &alarms, now_ms);

    uint32_t last_trend_ms = now_ms - 1000u;
    uint32_t last_wd_ms = now_ms;

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

        /* 1. Own heartbeat, and the UI heartbeat as seen from here */
        hal_atomic_store(&g_hb_fieldbus_ms, (int32_t)now_ms);
        hmi_watchdog_ping_fieldbus(&wd, now_ms);
        hmi_watchdog_ping_ui(&wd, (uint32_t)hal_atomic_load(&g_hb_ui_ms));

        /* 2. Operator commands (already authorised by the UI thread) */
        bool stall = false;
        int32_t bits = hal_atomic_exchange(&g_cmd_bits, 0);
        if (bits) fb_handle_commands(bits, &tags, &alarms, &wd, now_ms, &stall);
        fb_handle_device_commands(&tags, &alarms, now_ms);

        /* 3. Cyclic I/O */
        fb_attach_found(&tags, &alarms, now_ms);
        hmi_devmgr_poll(&tags, &alarms, now_ms);
        if (g_host_driver && g_host_driver->poll_cyclic)     g_host_driver->poll_cyclic(&tags, now_ms);

        /* 4. 1 Hz trend: one per device, of the tag its driver nominates (wrap-safe) */
        if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) {
            last_trend_ms += 1000u;
            if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) last_trend_ms = now_ms;   /* after a stall */
            static hmi_device_info_t devs[HMI_MAX_DEVICES];
            hmi_devmgr_publish(devs, now_ms);
            for (uint8_t d = 0; d < HMI_MAX_DEVICES; d++) {
                if (!hmi_device_present(&devs[d]) || devs[d].profile->trend_tag == HMI_REL_NONE) continue;
                uint8_t id = HMI_DEV_TAG(d, devs[d].profile->trend_tag);
                uint8_t tq = hmi_tag_read_quality(&tags, id);
                hmi_tags_push_trend(&tags, d, (tq == QUALITY_GOOD || tq == QUALITY_SIMULATED)
                                              ? hmi_tag_read_value(&tags, id) : HMI_TREND_GAP);
            }
        }

        /* 5. Alarms */
        hmi_alarms_evaluate(&alarms, &tags, now_ms);

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

        /* 8. Publish (CRC lets the UI verify the snapshot end to end) */
        st.publish_ms = now_ms;
        st.pacer_overruns = pacer.overruns;
        st.snapshot_crc = hmi_crc32c((const uint8_t *)&tags, sizeof(tags));
        fb_publish(&tags, &alarms, &wd, &st, now_ms);

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

typedef struct { bool active; uint8_t slot, action; int16_t value; bool has_value; } ui_cmd_t;
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
            if (pend.active)        /* carry on with the control that asked for the sign-in */
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
    g_cf_danger = a->kind == HMI_ACT_COIL_OFF || a->command == HMI_CMD_RAW_WRITE;
    g_pending_confirm = *c;
    g_pending_confirm.active = true;
    g_modal = HMI_MODAL_CONFIRM;
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
    if (a->kind == HMI_ACT_COIL_ON && a->block_tag != HMI_REL_NONE &&
        g_snap.tags.tags[HMI_DEV_TAG(slot, a->block_tag)].value != 0) {
        const hmi_tag_meta_t *bm = hmi_tag_get_meta(HMI_DEV_TAG(slot, a->block_tag));
        snprintf(msg, sizeof(msg), "%s blocked: %s is active", a->label, bm ? bm->name : "interlock");
        ui_toast(msg, now_ms);
        return;
    }
    if (a->kind == HMI_ACT_SET_REG) { ui_open_numpad(slot, action); return; }
    ui_cmd_t c = { true, slot, action, 0, false };
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
        ui_cmd_t c = { true, g_np_slot, g_np_action, (int16_t)v, true };
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
static void ui_device_action(uint8_t abs_tag, uint32_t now_ms)
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

static bool ui_banner_active(void)
{
    return g_have_snapshot && g_snap.alarms.current_action.active;
}

static void ui_dispatch_hit(const hmi_hit_t *hit, uint32_t now_ms)
{
    switch (hit->kind) {
        case HMI_HIT_NAV_TAB:
            if (hit->param < SCREEN_COUNT) g_active_screen = hit->param;
            break;
        case HMI_HIT_ESTOP:         ui_request(HMI_CMD_ESTOP, CMD_BIT_ESTOP, now_ms); break;
        case HMI_HIT_THEME:
        case HMI_HIT_SET_THEME:     ui_toggle_theme(); break;
        case HMI_HIT_ROLE:
        case HMI_HIT_SET_ROLE:      ui_open_role_dialog(now_ms); break;
        case HMI_HIT_MODAL_ROLE:    ui_choose_role(hit->param, now_ms); break;
        case HMI_HIT_MODAL_KEY:     ui_pin_key(hit->param, now_ms); break;
        case HMI_HIT_MODAL_CANCEL:
            if (g_modal == HMI_MODAL_FACEPLATE) g_fp_slot = NO_SLOT;    /* Close: leave the faceplate */
            modal_close();
            break;
        case HMI_HIT_MODAL_BLOCK:   break;
        case HMI_HIT_DEVICE_OPEN:   ui_open_faceplate(hit->param); break;
        case HMI_HIT_FP_CTRL: {
            const hmi_device_info_t *d = ui_dev(g_fp_slot);
            if (!d) break;
            hmi_fp_ctrl_t cs[HMI_FP_MAX_CTRLS];
            uint8_t n = hmi_profile_fp_controls(d->profile, cs, HMI_FP_MAX_CTRLS);
            if (hit->param < n) ui_start_action(g_fp_slot, cs[hit->param].action, cs[hit->param].option, now_ms);
            break;
        }
        case HMI_HIT_NUMPAD_KEY:    ui_numpad_key(hit->param, now_ms); break;
        case HMI_HIT_CONFIRM_OK:
            if (g_pending_confirm.active) {
                ui_cmd_t c = g_pending_confirm;
                modal_close();
                ui_send(&c, now_ms);
            }
            break;
        case HMI_HIT_BANNER_ACK:
        case HMI_HIT_ALARM_ACK:     ui_request(HMI_CMD_ACK_ALARMS, CMD_BIT_ACK, now_ms); break;
        case HMI_HIT_ALARM_SILENCE: ui_request(HMI_CMD_SILENCE_HORN, CMD_BIT_SILENCE, now_ms); break;
        case HMI_HIT_ESTOP_RESET:   ui_request(HMI_CMD_ESTOP_RESET, CMD_BIT_ESTOP_RESET, now_ms); break;
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

static void ui_handle_key(uint16_t key, bool shift, uint32_t now_ms)
{
    if (key == 'E') {                     /* E-stop works in every state, dialog or not */
        ui_request(HMI_CMD_ESTOP, CMD_BIT_ESTOP, now_ms);
        return;
    }
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
        } else if (g_modal == HMI_MODAL_CONFIRM && key == HMI_K_ENTER && g_pending_confirm.active) {
            ui_cmd_t c = g_pending_confirm;
            modal_close();
            ui_send(&c, now_ms);
        }
        return;
    }
    switch (key) {
        case HMI_K_TAB:    ui_navigate(shift ? -1 : 1); break;
        case HMI_K_RIGHT:  ui_navigate(1); break;
        case HMI_K_LEFT:   ui_navigate(-1); break;
        case '1': case '2': case '3': case '4': case '5':
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
        memcpy(&g_snap, &g_shared, sizeof(g_snap));
        tries++;
    } while (hmi_seqlock_read_retry(&g_seqlock, seq));
    g_seqlock_retries += tries - 1u;

    if (g_snap.fb_stats.publish_ms != 0 || g_snap.fb_stats.loop_count != 0) {
        g_have_snapshot = true;
        uint32_t crc = hmi_crc32c((const uint8_t *)&g_snap.tags, sizeof(g_snap.tags));
        if (crc != g_snap.fb_stats.snapshot_crc) g_crc_mismatches++;
    }

    g_ndev = 0;
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) {
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
    v->ui_frame_hz     = g_ui_frame_hz;
    v->exe_size_bytes  = g_exe_size;
    v->now_ms          = now_ms;
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
}

int app_width(void)  { return g_renderer.screen_w; }
int app_height(void) { return g_renderer.screen_h; }

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
    bool cf_gone = g_modal == HMI_MODAL_CONFIRM && !ui_dev(g_pending_confirm.slot);
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

    /* Horn: 1 s pulses while unacknowledged alarms exist */
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
    if (g == GESTURE_SWIPE_LEFT && g_modal == HMI_MODAL_NONE) {
        ui_navigate(1);
    } else if (g == GESTURE_SWIPE_RIGHT && g_modal == HMI_MODAL_NONE) {
        ui_navigate(-1);
    } else if (g == GESTURE_TAP) {
        uint8_t ntok = 0;
        const uint32_t *tok = ui_active_tokens(&ntok);
        hmi_hit_t hit = hmi_layout_hit_test(g_renderer.screen_w, g_renderer.screen_h, g_active_screen,
                                            ui_banner_active(), g_modal, tok, ntok,
                                            g_dev_view, g_ndev, g_fp_slot, tx, ty);
        ui_dispatch_hit(&hit, now_ms);
    }
    hal_request_repaint();
}

void app_pointer_cancel(void)
{
    hmi_gesture_cancel(&g_gesture);
}

bool app_pointer_active(void)
{
    return g_gesture.active;
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
/* --- Credential vault (security.vault): 144-byte CRC-protected blob --- */
/* 1 = loaded, 0 = no vault file, -1 = present but damaged */
static int vault_load(hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE + 1];
    hal_file_t *f = hal_file_open(g_vault_path, HAL_FILE_READ);
    if (!f) return 0;
    int got = hal_file_read(f, buf, sizeof(buf));
    hal_file_close(f);
    if (got != HMI_VAULT_SIZE) return -1;
    return hmi_security_vault_import(sec, buf, (size_t)got) ? 1 : -1;
}

static bool vault_save(const hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE];
    char tmp[HMI_PATH_MAX + 8];
    if (hmi_security_vault_export(sec, buf, sizeof(buf)) != HMI_VAULT_SIZE) return false;
    snprintf(tmp, sizeof(tmp), "%.255s.tmp", g_vault_path);
    hal_file_t *f = hal_file_open(tmp, HAL_FILE_WRITE_NEW);
    if (!f) return false;
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
    char out[200];
    hmi_security_init(&sec);
    if (vault_load(&sec) < 0) {
        hal_console_print("existing vault is damaged; starting from factory PINs\r\n");
        hmi_security_load_default_pins(&sec);
    }
    uint8_t salt[8];
    if (!hal_random(salt, sizeof(salt))) { hal_console_print("no OS random source available\r\n"); return 3; }
    if (!hmi_security_set_pin(&sec, (hmi_role_t)g_set_pin_role, g_set_pin_value, salt)) {
        hal_console_print("invalid PIN: use 4 to 8 digits\r\n");
        return 64;
    }
    hal_secure_zero(g_set_pin_value, sizeof(g_set_pin_value));
    if (!vault_save(&sec)) {
        snprintf(out, sizeof(out), "could not write %.150s\r\n", g_vault_path);
        hal_console_print(out);
        return 3;
    }
    snprintf(out, sizeof(out), "PIN for %s updated in %.150s\r\n", hmi_security_role_name((hmi_role_t)g_set_pin_role), g_vault_path);
    hal_console_print(out);
    return 0;
}

int app_parse_args(int argc, char **argv)
{
    exe_dir_path(g_log_path, sizeof(g_log_path), HMI_ALARM_LOG_PATH);
    exe_dir_path(g_vault_path, sizeof(g_vault_path), "security.vault");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--plc") == 0 && next) {
            if (!hmi_discovery_add(next, true)) {
                char out[160];
                snprintf(out, sizeof(out), "bad --plc address: %.100s (use ip[:port][/uN])\r\n", next);
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
        } else if (strcmp(a, "--sim") == 0) {
            static const char * const names[] = { "tank", "drive", "conveyor", "boiler", "pumps", "compressor", "ahu", "abb" };
            static const uint8_t types[] = { SIM_TANK, SIM_DRIVE, SIM_CONVEYOR, SIM_BOILER, SIM_PUMPS, SIM_COMPRESSOR, SIM_AHU, SIM_VFD };
            uint8_t t = SIM_TANK;
            if (next && next[0] != '-') {
                int found = -1;
                for (int k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) if (ieq(next, names[k])) found = k;
                if (found < 0) {
                    hal_console_print("unknown --sim type: use tank, drive, conveyor, boiler, pumps, compressor, ahu or abb\r\n");
                    return 64;
                }
                t = types[found];
                i++;
            }
            if (g_nsim < HMI_MAX_DEVICES) g_sim_types[g_nsim++] = t;
        } else if (strcmp(a, "--no-discovery") == 0) {
            g_discovery_on = false;
        } else if (strcmp(a, "--remove-after-ms") == 0 && next) {
            g_remove_after_ms = (uint32_t)strtoul(next, NULL, 10); i++;
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
        } else if (strcmp(a, "--set-pin") == 0 && next && i + 2 < argc) {
            g_set_pin_role = parse_role(next);
            copy_str(g_set_pin_value, sizeof(g_set_pin_value), argv[i + 2]);
            if (g_set_pin_role < 0 || strlen(argv[i + 2]) > HMI_PIN_MAX_LEN) {
                hal_console_print("usage: --set-pin <operator|maintenance|engineer> <4-8 digit PIN>\r\n");
                return 64;
            }
            i += 2;
        } else if (strcmp(a, "--verify-log") == 0 && next) {
            uint32_t lines = 0, bad = 0, first_bad = 0;
            bool ok = hmi_alarm_log_verify_file(next, &lines, &bad, &first_bad);
            char out[320];
            if (ok) {
                snprintf(out, sizeof(out), "%.200s: %lu lines, hash chain intact -> VERIFIED\r\n",
                         next, (unsigned long)lines);
            } else if (lines == 0) {
                snprintf(out, sizeof(out), "%.200s: cannot read file or file is empty -> FAILED\r\n", next);
            } else {
                snprintf(out, sizeof(out), "%.200s: %lu lines, %lu fail, chain broken at line %lu -> TAMPERED\r\n",
                         next, (unsigned long)lines, (unsigned long)bad, (unsigned long)first_bad);
            }
            hal_console_print(out);
            return ok ? 0 : 2;
        } else {
            char out[160];
            snprintf(out, sizeof(out), "unknown or incomplete argument: %s\r\n", a);
            hal_console_print(out);
            return 64;
        }
    }
    if (g_set_pin_role >= 0) return run_set_pin();
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
    char devs[400];
    size_t dl = 0;
    devs[0] = '\0';
    for (int d = 0; d < HMI_MAX_DEVICES; d++) {
        const hmi_device_info_t *dv = &g_snap.devices[d];
        int k = snprintf(devs + dl, sizeof(devs) - dl, "device%d=%s|%s|%s|state%u|ok%lu|to%lu\r\n", d,
                         dv->state ? dv->label : "-", dv->state ? dv->endpoint : "-",
                         dv->profile ? dv->profile->key : "-", (unsigned)dv->state,
                         (unsigned long)dv->responses, (unsigned long)dv->timeouts);
        if (k > 0 && (size_t)k < sizeof(devs) - dl) dl += (size_t)k;
    }
    char buf[1800];
    int n = snprintf(buf, sizeof(buf),
        "fieldbus_loop_hz=%lu\r\nfieldbus_period_avg_us=%lu\r\nfieldbus_period_min_us=%lu\r\n"
        "fieldbus_period_max_us=%lu\r\nfieldbus_late_count=%lu\r\nfieldbus_loop_count=%lu\r\n"
        "fieldbus_high_res_timer=%d\r\npacer_overruns=%lu\r\nui_frame_hz=%lu\r\nui_max_frame_ms=%lu\r\n"
        "seqlock_retries=%lu\r\ncrc_mismatches=%lu\r\nui_watchdog_faults=%lu\r\n"
        "ui_wd_fieldbus_alive=%d\r\nfb_wd_ui_alive=%d\r\nfb_wd_fault_count=%lu\r\n"
        "devices_present=%u\r\n%s"
        "alarm_latch=%d\r\nalarm_highest=%d\r\nalarm_active=%u\r\nalarm_unacked=%u\r\njournal_total=%lu\r\n"
        "log_open=%d\r\nlog_records_written=%lu\r\nlog_records_lost=%lu\r\nlog_write_errors=%lu\r\n"
        "role=%s\r\naccess_denied=%lu\r\nexe_size=%lu\r\nbuild=%s\r\n",
        (unsigned long)fb->loop_hz, (unsigned long)fb->period_avg_us, (unsigned long)fb->period_min_us,
        (unsigned long)fb->period_max_us, (unsigned long)fb->late_count, (unsigned long)fb->loop_count,
        fb->high_res_timer ? 1 : 0, (unsigned long)fb->pacer_overruns, (unsigned long)g_ui_frame_hz,
        (unsigned long)g_ui_max_frame_ms, (unsigned long)g_seqlock_retries, (unsigned long)g_crc_mismatches,
        (unsigned long)g_ui_wd_faults_seen, g_ui_wd.fieldbus_alive ? 1 : 0, g_snap.fb_watchdog.ui_alive ? 1 : 0,
        (unsigned long)g_snap.fb_watchdog.fault_count,
        (unsigned)g_ndev, devs,
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
    hmi_timer_init();
    uint32_t now_ms = hmi_timer_get_ms();

    hmi_security_init(&g_security);                     /* starts as VIEWER with factory PINs */
    hmi_security_register_activity(&g_security, now_ms);
    int vault = vault_load(&g_security);
    if (vault < 0) {
        hmi_security_load_default_pins(&g_security);
        ui_post_event(ALARM_SEV_WARNING, "Credential vault damaged: factory PINs");
        ui_toast("Credential vault damaged - factory PINs in use", now_ms);
    }
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
