/**
 * @file main.c
 * @brief Industrial HMI runtime: UI thread (Win32, ~30 Hz) + fieldbus thread (1 kHz)
 * @standards IEC 62443-4-2 (RBAC) / ISA-18.2 (alarms) / ISA-101 (display)
 *
 * Threads and ownership
 *   Fieldbus thread  owns the tag table, the alarm supervisor (and its journal),
 *                    the PLC drivers and a watchdog that supervises the UI heartbeat.
 *                    Publishes everything once per iteration through a seqlock.
 *   UI thread        owns the window, renderer, RBAC context, the persistent alarm
 *                    log, and a watchdog that supervises the fieldbus heartbeat.
 *                    Reads only the latest seqlock snapshot (g_snap).
 *   UI -> fieldbus   operator commands as bits in one atomic word, and audit
 *                    events (role changes, access denials, ...) through a
 *                    single-producer/single-consumer ring.
 *
 * Command line
 *   --plc <ip[:port][/unit]>   use Modbus TCP instead of the built-in simulator
 *   --log <path>               alarm log file (default alarms.log next to the exe)
 *   --no-log                   disable the persistent alarm log
 *   --exit-after-ms <n>        close automatically after n ms (tests / measure.bat)
 *   --report <path>            write measured runtime statistics on exit
 *   --verify-log <path>        verify an alarm log's CRCs, print result, exit (0 = ok)
 */

#define _CRT_RAND_S              /* rand_s(): OS random numbers for PIN salts */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
#include "../include/drivers/driver_modbus.h"
#include "../include/drivers/driver_host.h"
#include "../include/hal/timer.h"
#include "../include/hal/display.h"
#include "../include/hal/gesture.h"

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
    hmi_driver_stats_t     plc_stats;
} hmi_shared_state_t;

static hmi_shared_state_t g_shared;       /* written only by the fieldbus thread */
static hmi_seqlock_t      g_seqlock;
static hmi_shared_state_t g_snap;         /* UI thread's private copy */

/* Heartbeats (uptime ms), each written by one thread, read by the other */
static volatile LONG g_hb_ui_ms;
static volatile LONG g_hb_fieldbus_ms;
static volatile LONG g_running = 1;

/* UI -> fieldbus command bits */
#define CMD_BIT_ACK          (1L << 0)
#define CMD_BIT_SILENCE      (1L << 1)
#define CMD_BIT_ESTOP        (1L << 2)
#define CMD_BIT_ESTOP_RESET  (1L << 3)
#define CMD_BIT_PUMP_TOGGLE  (1L << 4)
#define CMD_BIT_VALVE_TOGGLE (1L << 5)
#define CMD_BIT_INJECT_FAULT (1L << 6)
#define CMD_BIT_WD_RESET     (1L << 7)
static volatile LONG g_cmd_bits;

/* UI -> fieldbus audit events (SPSC ring: UI produces, fieldbus consumes) */
#define UI_EVENT_RING 16
typedef struct {
    uint8_t severity;
    char    msg[41];
} ui_event_t;
static ui_event_t    g_ui_events[UI_EVENT_RING];
static volatile LONG g_ui_ev_head;   /* written by UI */
static volatile LONG g_ui_ev_tail;   /* written by fieldbus */
static volatile LONG g_ui_ev_dropped;

/* Drivers (set in WinMain before the fieldbus thread starts; read-only afterwards) */
static const hmi_plc_driver_t *g_modbus_driver;
static const hmi_plc_driver_t *g_host_driver;

/* System alarm points owned by the runtime itself */
#define SYS_TAG_WATCHDOG 13
static const hmi_tag_meta_t s_system_meta[] = {
    { SYS_TAG_WATCHDOG, TAG_TYPE_DIGITAL, "HMI watchdog failover", "", 0, 1, 0, 0, 0, ALARM_SEV_CRITICAL,
      "A control loop stalled; check the HMI host for overload", "Maintenance: reset the failover latch in Settings" },
};
static char g_plc_endpoint[64];

/* ------------------------------------------------------------------------- */
/* UI-thread state                                                            */
/* ------------------------------------------------------------------------- */
static hmi_renderer_t        g_renderer;
static hal_display_t         g_display;
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
static char                  g_log_path[MAX_PATH];
static uint32_t              g_exit_after_ms;
static char                  g_report_path[MAX_PATH];
static char                  g_vault_path[MAX_PATH];
static int                   g_set_pin_role = -1;
static char                  g_set_pin_value[HMI_PIN_MAX_LEN + 1];
static uint32_t              g_ui_max_frame_ms;
static uint32_t              g_ui_wd_faults_seen;

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
    LONG head = g_ui_ev_head;
    LONG tail = InterlockedCompareExchange(&g_ui_ev_tail, 0, 0);   /* atomic read */
    if ((uint32_t)(head - tail) >= UI_EVENT_RING) {
        InterlockedIncrement(&g_ui_ev_dropped);
        return;
    }
    ui_event_t *ev = &g_ui_events[(uint32_t)head % UI_EVENT_RING];
    ev->severity = severity;
    copy_str(ev->msg, sizeof(ev->msg), msg);
    InterlockedExchange(&g_ui_ev_head, head + 1);                   /* publish (full barrier) */
}

static void fb_drain_ui_events(hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    LONG tail = g_ui_ev_tail;
    LONG head = InterlockedCompareExchange(&g_ui_ev_head, 0, 0);
    while (tail != head) {
        const ui_event_t *ev = &g_ui_events[(uint32_t)tail % UI_EVENT_RING];
        hmi_alarms_log_event(alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ev->severity, ALARM_PT_NORMAL, ev->msg);
        tail++;
        InterlockedExchange(&g_ui_ev_tail, tail);
    }
    LONG dropped = InterlockedExchange(&g_ui_ev_dropped, 0);
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

static void fb_handle_commands(LONG bits, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms,
                               hmi_watchdog_t *wd, uint32_t now_ms, bool *stall_requested)
{
    const hmi_plc_driver_t *plc = g_modbus_driver;

    if (bits & CMD_BIT_ESTOP) {
        bool ok = plc && plc->write_coil && plc->write_coil(MB_COIL_ESTOP, true);
        hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_ESTOP_ACTIVE, ALARM_SEV_ESTOP, ALARM_PT_NORMAL,
                             ok ? "E-stop commanded by operator" : "E-stop write failed: use hardwired E-stop");
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
        const hmi_alarm_point_t *pt = &alarms->points[SCHNEIDER_TAG_ESTOP_ACTIVE];
        if (pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_RTN_UNACK) {
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_ESTOP_ACTIVE, ALARM_SEV_WARNING, pt->state,
                                 "E-stop reset refused: acknowledge first");
        } else if (hmi_tag_read_value(tags, SCHNEIDER_TAG_ESTOP_ACTIVE) == 0) {
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_ESTOP_ACTIVE, ALARM_SEV_INFO, pt->state,
                                 "E-stop reset: not tripped");
        } else {
            bool ok = plc && plc->write_coil && plc->write_coil(MB_COIL_ESTOP, false);
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_ESTOP_ACTIVE, ALARM_SEV_INFO, pt->state,
                                 ok ? "E-stop reset commanded" : "E-stop reset write failed");
        }
    }
    if (bits & CMD_BIT_PUMP_TOGGLE) {
        bool running = hmi_tag_read_value(tags, SCHNEIDER_TAG_PUMP_RUNNING) != 0;
        bool estop = hmi_tag_read_value(tags, SCHNEIDER_TAG_ESTOP_ACTIVE) != 0;
        if (!running && estop) {
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_PUMP_RUNNING, ALARM_SEV_WARNING, ALARM_PT_NORMAL,
                                 "Pump start blocked: E-stop active");
        } else if (hmi_tag_read_quality(tags, SCHNEIDER_TAG_PUMP_RUNNING) != QUALITY_GOOD &&
                   hmi_tag_read_quality(tags, SCHNEIDER_TAG_PUMP_RUNNING) != QUALITY_SIMULATED) {
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_PUMP_RUNNING, ALARM_SEV_WARNING, ALARM_PT_NORMAL,
                                 "Pump command blocked: status unknown");
        } else {
            bool ok = plc && plc->write_coil && plc->write_coil(MB_COIL_PUMP_A, !running);
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_PUMP_RUNNING, ALARM_SEV_INFO, ALARM_PT_NORMAL,
                                 ok ? (running ? "Pump A stop commanded" : "Pump A start commanded")
                                    : "Pump A write failed");
        }
    }
    if (bits & CMD_BIT_VALVE_TOGGLE) {
        uint8_t q = hmi_tag_read_quality(tags, SCHNEIDER_TAG_VALVE_POS);
        if (q != QUALITY_GOOD && q != QUALITY_SIMULATED) {
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_VALVE_POS, ALARM_SEV_WARNING, ALARM_PT_NORMAL,
                                 "Valve command blocked: position unknown");
        } else {
            int16_t target = hmi_valve_toggle_target(hmi_tag_read_value(tags, SCHNEIDER_TAG_VALVE_POS));
            bool ok = plc && plc->write_register && plc->write_register(MB_HR_VALVE_POS, target);
            char msg[41];
            snprintf(msg, sizeof(msg), ok ? "Valve V-101 move to %d %%" : "Valve V-101 write failed (%d %%)", (int)target);
            hmi_alarms_log_event(alarms, now_ms, SCHNEIDER_TAG_VALVE_POS, ALARM_SEV_INFO, ALARM_PT_NORMAL, msg);
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

static void fb_publish(const hmi_tag_table_t *tags, const hmi_alarm_supervisor_t *alarms,
                       const hmi_watchdog_t *wd, const hmi_fieldbus_stats_t *st)
{
    hmi_seqlock_write_begin(&g_seqlock);
    g_shared.tags        = *tags;
    g_shared.alarms      = *alarms;
    g_shared.fb_watchdog = *wd;
    g_shared.fb_stats    = *st;
    if (g_modbus_driver && g_modbus_driver->get_stats) {
        g_modbus_driver->get_stats(&g_shared.plc_stats);
    }
    hmi_seqlock_write_end(&g_seqlock);
}

static DWORD WINAPI FieldbusThreadProc(LPVOID param)
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

    if (g_modbus_driver && g_modbus_driver->connect) {
        g_modbus_driver->connect(g_plc_endpoint[0] ? g_plc_endpoint : NULL);
    }
    hmi_alarms_log_event(&alarms, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL,
                         g_plc_endpoint[0] ? "HMI started (Modbus TCP)" : "HMI started (simulator)");

    uint32_t last_trend_ms = now_ms - 1000u;
    uint32_t last_wd_ms = now_ms;

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
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
        InterlockedExchange(&g_hb_fieldbus_ms, (LONG)now_ms);
        hmi_watchdog_ping_fieldbus(&wd, now_ms);
        hmi_watchdog_ping_ui(&wd, (uint32_t)InterlockedCompareExchange(&g_hb_ui_ms, 0, 0));

        /* 2. Operator commands (already authorised by the UI thread) */
        bool stall = false;
        LONG bits = InterlockedExchange(&g_cmd_bits, 0);
        if (bits) fb_handle_commands(bits, &tags, &alarms, &wd, now_ms, &stall);

        /* 3. Cyclic I/O */
        if (g_modbus_driver && g_modbus_driver->poll_cyclic) g_modbus_driver->poll_cyclic(&tags, now_ms);
        if (g_host_driver && g_host_driver->poll_cyclic)     g_host_driver->poll_cyclic(&tags, now_ms);

        /* 4. 1 Hz trend of tank level (wrap-safe) */
        if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) {
            last_trend_ms += 1000u;
            if ((uint32_t)(now_ms - last_trend_ms) >= 1000u) last_trend_ms = now_ms;   /* after a stall */
            uint8_t tq = hmi_tag_read_quality(&tags, SCHNEIDER_TAG_TANK_LEVEL);
            hmi_tags_push_trend(&tags, (tq == QUALITY_GOOD || tq == QUALITY_SIMULATED)
                                       ? hmi_tag_read_value(&tags, SCHNEIDER_TAG_TANK_LEVEL) : HMI_TREND_GAP);
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
        fb_publish(&tags, &alarms, &wd, &st);

        if (stall) {
            /* Deliberate fault: stop heartbeating so the UI-side watchdog must catch it. */
            Sleep(HMI_FAULT_INJECT_STALL_MS);
        }

        hmi_pacer_wait(&pacer);
    }

    if (g_modbus_driver && g_modbus_driver->disconnect) g_modbus_driver->disconnect();
    hmi_pacer_close(&pacer);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* UI: commands, input                                                        */
/* ------------------------------------------------------------------------- */
static void ui_request(hmi_command_t cmd, LONG bit, uint32_t now_ms)
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
    if (bit) InterlockedOr(&g_cmd_bits, bit);
}

/* --- Sign-in dialogs (PIN digits live only here, never in the view) --- */
static uint8_t g_modal = HMI_MODAL_NONE;
static uint8_t g_modal_role;
static char    g_pin[HMI_PIN_MAX_LEN + 1];
static uint8_t g_pin_len;
static const char *g_modal_msg;
static bool    g_modal_msg_error;

static void pin_clear(void)
{
    SecureZeroMemory(g_pin, sizeof(g_pin));
    g_pin_len = 0;
}

static void modal_close(void)
{
    pin_clear();
    g_modal = HMI_MODAL_NONE;
    g_modal_msg = NULL;
}

static void ui_open_role_dialog(uint32_t now_ms)
{
    if (!hmi_security_authorize(&g_security, HMI_CMD_ROLE_SELECT, now_ms)) return;
    pin_clear();
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

static void ui_pin_submit(uint32_t now_ms)
{
    static char msg[48];
    hmi_role_t role = (hmi_role_t)g_modal_role;
    hmi_login_result_t res = hmi_security_login(&g_security, role, g_pin, now_ms);
    pin_clear();
    char ev[41];
    switch (res) {
        case HMI_LOGIN_OK:
            snprintf(ev, sizeof(ev), "Signed in: %s", hmi_security_role_name(role));
            ui_post_event(ALARM_SEV_INFO, ev);
            snprintf(msg, sizeof(msg), "Signed in as %s", hmi_security_role_name(role));
            ui_toast(msg, now_ms);
            modal_close();
            break;
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
        g_modal_msg = NULL;
        g_modal_msg_error = false;
    } else if (key == HMI_KEY_DELETE) {
        if (g_pin_len) g_pin[--g_pin_len] = '\0';
    } else if (key == HMI_KEY_OK) {
        ui_pin_submit(now_ms);
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
    InterlockedOr(&g_cmd_bits, CMD_BIT_WD_RESET);
    ui_post_event(ALARM_SEV_INFO, ok ? "Watchdog failover reset" : "Watchdog reset refused: not healthy");
    ui_toast(ok ? "Watchdog failover latch reset" : "Watchdog reset refused: loop not healthy", now_ms);
}

static const uint32_t* ui_active_tokens(uint8_t *count)
{
    *count = 0;
    if (g_active_screen == SCREEN_PROCESS_OVERVIEW && g_modbus_driver) {
        *count = g_modbus_driver->widget_count;
        return g_modbus_driver->ui_bytecode;
    }
    if (g_active_screen == SCREEN_HOST_TELEMETRY && g_host_driver) {
        *count = g_host_driver->widget_count;
        return g_host_driver->ui_bytecode;
    }
    return NULL;
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
        case HMI_HIT_MODAL_CANCEL:  modal_close(); break;
        case HMI_HIT_MODAL_BLOCK:   break;
        case HMI_HIT_BANNER_ACK:
        case HMI_HIT_ALARM_ACK:     ui_request(HMI_CMD_ACK_ALARMS, CMD_BIT_ACK, now_ms); break;
        case HMI_HIT_ALARM_SILENCE: ui_request(HMI_CMD_SILENCE_HORN, CMD_BIT_SILENCE, now_ms); break;
        case HMI_HIT_ESTOP_RESET:   ui_request(HMI_CMD_ESTOP_RESET, CMD_BIT_ESTOP_RESET, now_ms); break;
        case HMI_HIT_SET_WD_TEST:   ui_request(HMI_CMD_INJECT_FAULT, CMD_BIT_INJECT_FAULT, now_ms); break;
        case HMI_HIT_SET_WD_RESET:  ui_watchdog_reset(now_ms); break;
        case HMI_HIT_WIDGET_BUTTON:
            if (hit->param == SCHNEIDER_TAG_PUMP_RUNNING)   ui_request(HMI_CMD_PUMP_TOGGLE, CMD_BIT_PUMP_TOGGLE, now_ms);
            else if (hit->param == SCHNEIDER_TAG_VALVE_POS) ui_request(HMI_CMD_VALVE_TOGGLE, CMD_BIT_VALVE_TOGGLE, now_ms);
            break;
        case HMI_HIT_NONE:
        default:
            break;
    }
}

static void ui_handle_key(WPARAM vk, uint32_t now_ms)
{
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == 'E') {                      /* E-stop works in every state, dialog or not */
        ui_request(HMI_CMD_ESTOP, CMD_BIT_ESTOP, now_ms);
        return;
    }
    if (g_modal != HMI_MODAL_NONE) {      /* a dialog owns the keyboard */
        if (vk == VK_ESCAPE) { modal_close(); return; }
        if (g_modal == HMI_MODAL_ROLE && vk >= '1' && vk <= '4') { ui_choose_role((uint8_t)(vk - '1'), now_ms); return; }
        if (g_modal == HMI_MODAL_PIN) {
            if (vk >= '0' && vk <= '9') ui_pin_key((uint8_t)(vk - '0'), now_ms);
            else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ui_pin_key((uint8_t)(vk - VK_NUMPAD0), now_ms);
            else if (vk == VK_BACK) ui_pin_key(HMI_KEY_DELETE, now_ms);
            else if (vk == VK_RETURN) ui_pin_key(HMI_KEY_OK, now_ms);
        }
        return;
    }
    switch (vk) {
        case VK_TAB:    ui_navigate(shift ? -1 : 1); break;
        case VK_RIGHT:  ui_navigate(1); break;
        case VK_LEFT:   ui_navigate(-1); break;
        case '1': case '2': case '3': case '4': case '5':
            g_active_screen = (uint8_t)(vk - '1');
            break;
        case VK_SPACE:
        case 'A':       ui_request(HMI_CMD_ACK_ALARMS, CMD_BIT_ACK, now_ms); break;
        case 'S':       ui_request(HMI_CMD_SILENCE_HORN, CMD_BIT_SILENCE, now_ms); break;
        case 'P':       ui_request(HMI_CMD_PUMP_TOGGLE, CMD_BIT_PUMP_TOGGLE, now_ms); break;
        case 'V':       ui_request(HMI_CMD_VALVE_TOGGLE, CMD_BIT_VALVE_TOGGLE, now_ms); break;

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
    v->plc    = g_have_snapshot ? &g_snap.plc_stats : NULL;
    v->log    = &log_stats;
    if (g_modbus_driver) { v->tokens_process = g_modbus_driver->ui_bytecode; v->ntok_process = g_modbus_driver->widget_count; }
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
}

static void ui_paint(HDC hdc)
{
    hmi_view_t view;
    uint32_t now_ms = hmi_timer_get_ms();
    ui_build_view(&view, now_ms);
    const uint32_t *palette = hmi_renderer_get_palette(&g_renderer);

    for (int b = 0; b < g_renderer.band_count; b++) {
        hmi_renderer_render_band(&g_renderer, b, &view);
        int rows = g_renderer.band_h;
        if (g_renderer.current_band_y + rows > g_renderer.screen_h) rows = g_renderer.screen_h - g_renderer.current_band_y;
        if (rows <= 0) break;
        hal_display_present_band(hdc, hmi_renderer_get_band_buffer(&g_renderer),
                                 g_renderer.current_band_y, rows, g_renderer.screen_w, palette);
    }
}

/* ------------------------------------------------------------------------- */
/* Window procedure                                                           */
/* ------------------------------------------------------------------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   /* every pixel is painted by the band streamer */

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO *)lParam;
            RECT r = { 0, 0, HMI_LAYOUT_MIN_W, HMI_LAYOUT_MIN_H };
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }

        case WM_SIZE: {
            int w = LOWORD(lParam), h = HIWORD(lParam);
            if (wParam != SIZE_MINIMIZED && w > 0 && h > 0) {
                hmi_renderer_resize(&g_renderer, w, h);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            ui_paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            uint32_t now_ms = hmi_timer_get_ms();
            hmi_security_register_activity(&g_security, now_ms);
            SetCapture(hwnd);
            hmi_gesture_begin(&g_gesture, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), now_ms);
            return 0;
        }

        case WM_MOUSEMOVE:
            if (g_gesture.active) hmi_gesture_move(&g_gesture, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP: {
            uint32_t now_ms = hmi_timer_get_ms();
            int32_t tx = 0, ty = 0;
            hmi_gesture_t g = hmi_gesture_end(&g_gesture, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                                              now_ms, g_renderer.screen_w, &tx, &ty);
            if (GetCapture() == hwnd) ReleaseCapture();
            hmi_security_register_activity(&g_security, now_ms);
            if (g == GESTURE_SWIPE_LEFT && g_modal == HMI_MODAL_NONE) {
                ui_navigate(1);
            } else if (g == GESTURE_SWIPE_RIGHT && g_modal == HMI_MODAL_NONE) {
                ui_navigate(-1);
            } else if (g == GESTURE_TAP) {
                uint8_t ntok = 0;
                const uint32_t *tok = ui_active_tokens(&ntok);
                hmi_hit_t hit = hmi_layout_hit_test(g_renderer.screen_w, g_renderer.screen_h, g_active_screen,
                                                    ui_banner_active(), g_modal, tok, ntok, tx, ty);
                ui_dispatch_hit(&hit, now_ms);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_CAPTURECHANGED:
            if ((HWND)lParam != hwnd) hmi_gesture_cancel(&g_gesture);
            return 0;

        case WM_KEYDOWN: {
            uint32_t now_ms = hmi_timer_get_ms();
            hmi_security_register_activity(&g_security, now_ms);
            if (lParam & (1L << 30)) return 0;   /* auto-repeat: one key press = one command */
            ui_handle_key(wParam, now_ms);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            InterlockedExchange(&g_running, 0);
            g_display.hwnd = NULL;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

/* ------------------------------------------------------------------------- */
/* Startup helpers                                                            */
/* ------------------------------------------------------------------------- */
static void console_print(const char *text)
{
    /* GUI-subsystem exe: attach to the parent console if there is one. */
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written;
        if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    }
}

static void exe_dir_path(char *out, size_t cap, const char *file)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) { copy_str(out, cap, file); return; }
    char *slash = strrchr(path, '\\');
    if (slash) slash[1] = '\0'; else path[0] = '\0';
    snprintf(out, cap, "%.200s%.50s", path, file);
}

static uint32_t exe_file_size(void)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (n == 0 || n >= sizeof(path)) return 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    return fad.nFileSizeHigh ? 0xFFFFFFFFu : fad.nFileSizeLow;
}

/* Returns -1 to continue, otherwise a process exit code. */
/* --- Credential vault (security.vault): 144-byte CRC-protected blob --- */
/* 1 = loaded, 0 = no vault file, -1 = present but damaged */
static int vault_load(hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE + 1];
    HANDLE h = CreateFileA(g_vault_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof(buf), &got, NULL);
    CloseHandle(h);
    if (!ok || got != HMI_VAULT_SIZE) return -1;
    return hmi_security_vault_import(sec, buf, got) ? 1 : -1;
}

static bool vault_save(const hmi_security_ctx_t *sec)
{
    uint8_t buf[HMI_VAULT_SIZE];
    char tmp[MAX_PATH + 8];
    if (hmi_security_vault_export(sec, buf, sizeof(buf)) != HMI_VAULT_SIZE) return false;
    snprintf(tmp, sizeof(tmp), "%.255s.tmp", g_vault_path);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, buf, sizeof(buf), &w, NULL) && w == sizeof(buf) && FlushFileBuffers(h);
    CloseHandle(h);
    /* atomic replace: a power cut leaves either the old or the new vault, never half of one */
    if (!ok || !MoveFileExA(tmp, g_vault_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
        return false;
    }
    return true;
}

static int parse_role(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "1") == 0 || _stricmp(s, "operator") == 0) return ROLE_OPERATOR;
    if (strcmp(s, "2") == 0 || _stricmp(s, "maintenance") == 0) return ROLE_MAINTENANCE;
    if (strcmp(s, "3") == 0 || _stricmp(s, "engineer") == 0) return ROLE_ENGINEER;
    return -1;
}

static int run_set_pin(void)
{
    static hmi_security_ctx_t sec;
    char out[200];
    hmi_security_init(&sec);
    if (vault_load(&sec) < 0) {
        console_print("existing vault is damaged; starting from factory PINs\r\n");
        hmi_security_load_default_pins(&sec);
    }
    uint8_t salt[8];
    for (int i = 0; i < 8; i += 4) {
        unsigned int r = 0;
        if (rand_s(&r) != 0) { console_print("no OS random source available\r\n"); return 3; }
        memcpy(salt + i, &r, 4);
    }
    if (!hmi_security_set_pin(&sec, (hmi_role_t)g_set_pin_role, g_set_pin_value, salt)) {
        console_print("invalid PIN: use 4 to 8 digits\r\n");
        return 64;
    }
    SecureZeroMemory(g_set_pin_value, sizeof(g_set_pin_value));
    if (!vault_save(&sec)) {
        snprintf(out, sizeof(out), "could not write %.150s\r\n", g_vault_path);
        console_print(out);
        return 3;
    }
    snprintf(out, sizeof(out), "PIN for %s updated in %.150s\r\n", hmi_security_role_name((hmi_role_t)g_set_pin_role), g_vault_path);
    console_print(out);
    return 0;
}

static int parse_args(int argc, char **argv)
{
    exe_dir_path(g_log_path, sizeof(g_log_path), HMI_ALARM_LOG_PATH);
    exe_dir_path(g_vault_path, sizeof(g_vault_path), "security.vault");
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--plc") == 0 && next) {
            copy_str(g_plc_endpoint, sizeof(g_plc_endpoint), next); i++;
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
                console_print("usage: --set-pin <operator|maintenance|engineer> <4-8 digit PIN>\r\n");
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
            console_print(out);
            return ok ? 0 : 2;
        } else {
            char out[160];
            snprintf(out, sizeof(out), "unknown or incomplete argument: %s\r\n", a);
            console_print(out);
            return 64;
        }
    }
    if (g_set_pin_role >= 0) return run_set_pin();
    return -1;
}

static void write_report(void)
{
    if (!g_report_path[0]) return;
    HANDLE h = CreateFileA(g_report_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    hmi_alarm_log_stats_t ls;
    hmi_alarm_log_get_stats(&ls);
    const hmi_fieldbus_stats_t *fb = &g_snap.fb_stats;
    const hmi_driver_stats_t *pl = &g_snap.plc_stats;
    char buf[1400];
    int n = snprintf(buf, sizeof(buf),
        "fieldbus_loop_hz=%lu\r\nfieldbus_period_avg_us=%lu\r\nfieldbus_period_min_us=%lu\r\n"
        "fieldbus_period_max_us=%lu\r\nfieldbus_late_count=%lu\r\nfieldbus_loop_count=%lu\r\n"
        "fieldbus_high_res_timer=%d\r\npacer_overruns=%lu\r\nui_frame_hz=%lu\r\nui_max_frame_ms=%lu\r\n"
        "seqlock_retries=%lu\r\ncrc_mismatches=%lu\r\nui_watchdog_faults=%lu\r\n"
        "ui_wd_fieldbus_alive=%d\r\nfb_wd_ui_alive=%d\r\nfb_wd_fault_count=%lu\r\n"
        "plc_mode=%u\r\nplc_link=%u\r\nplc_endpoint=%s\r\nplc_requests=%lu\r\nplc_responses_ok=%lu\r\n"
        "plc_timeouts=%lu\r\nplc_exceptions=%lu\r\nplc_protocol_errors=%lu\r\nplc_reconnects=%lu\r\n"
        "plc_writes_ok=%lu\r\nplc_writes_failed=%lu\r\nplc_last_rtt_us=%lu\r\nplc_max_rtt_us=%lu\r\n"
        "alarm_latch=%d\r\nalarm_highest=%d\r\nalarm_active=%u\r\nalarm_unacked=%u\r\njournal_total=%lu\r\n"
        "log_open=%d\r\nlog_records_written=%lu\r\nlog_records_lost=%lu\r\nlog_write_errors=%lu\r\n"
        "role=%s\r\naccess_denied=%lu\r\nexe_size=%lu\r\nbuild=%s\r\n",
        (unsigned long)fb->loop_hz, (unsigned long)fb->period_avg_us, (unsigned long)fb->period_min_us,
        (unsigned long)fb->period_max_us, (unsigned long)fb->late_count, (unsigned long)fb->loop_count,
        fb->high_res_timer ? 1 : 0, (unsigned long)fb->pacer_overruns, (unsigned long)g_ui_frame_hz,
        (unsigned long)g_ui_max_frame_ms, (unsigned long)g_seqlock_retries, (unsigned long)g_crc_mismatches,
        (unsigned long)g_ui_wd_faults_seen, g_ui_wd.fieldbus_alive ? 1 : 0, g_snap.fb_watchdog.ui_alive ? 1 : 0,
        (unsigned long)g_snap.fb_watchdog.fault_count,
        (unsigned)pl->mode, (unsigned)pl->link_state, pl->endpoint, (unsigned long)pl->requests_sent,
        (unsigned long)pl->responses_ok, (unsigned long)pl->timeouts, (unsigned long)pl->exceptions,
        (unsigned long)pl->protocol_errors, (unsigned long)pl->reconnects, (unsigned long)pl->writes_ok,
        (unsigned long)pl->writes_failed, (unsigned long)pl->last_rtt_us, (unsigned long)pl->max_rtt_us,
        (int)g_snap.alarms.latch_state, (int)g_snap.alarms.highest_severity,
        (unsigned)g_snap.alarms.active_count, (unsigned)g_snap.alarms.unacked_count,
        (unsigned long)g_snap.alarms.journal_total,
        ls.open ? 1 : 0, (unsigned long)ls.records_written, (unsigned long)ls.records_lost,
        (unsigned long)ls.write_errors,
        hmi_security_role_name(g_security.current_role), (unsigned long)g_security.denied_count,
        (unsigned long)g_exe_size, g_hmi_build_info);
    if (n > 0) {
        DWORD written;
        WriteFile(h, buf, (DWORD)((size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1), &written, NULL);
    }
    CloseHandle(h);
}

/* ------------------------------------------------------------------------- */
/* Crash record: last-resort diagnostics written next to the exe              */
/* ------------------------------------------------------------------------- */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char path[MAX_PATH], line[256];
    exe_dir_path(path, sizeof(path), "crash.log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE && ep && ep->ExceptionRecord) {
        uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
        uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        int n = snprintf(line, sizeof(line), "crash: code=0x%08lX offset=0x%llX thread=%lu uptime_ms=%lu\r\n",
                         (unsigned long)ep->ExceptionRecord->ExceptionCode,
                         (unsigned long long)(addr - base), (unsigned long)GetCurrentThreadId(),
                         (unsigned long)hmi_timer_get_ms());
        DWORD w;
        if (n > 0) WriteFile(h, line, (DWORD)n, &w, NULL);
        CloseHandle(h);
    }
    hmi_alarm_log_close();      /* flush what we have */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                */
/* ------------------------------------------------------------------------- */
static int hmi_run(HINSTANCE hInstance, int argc, char **argv)
{
    SetUnhandledExceptionFilter(crash_filter);
    int rc = parse_args(argc, argv);
    if (rc >= 0) return rc;

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
    g_modbus_driver = hmi_driver_modbus_get();
    g_host_driver   = hmi_driver_host_get();
    hmi_tag_registry_reset();
    bool meta_ok = true;
    if (g_modbus_driver) {
        meta_ok &= hmi_tag_register_meta(g_modbus_driver->tag_metadata, g_modbus_driver->tag_count);
        if (g_modbus_driver->init) g_modbus_driver->init();
    }
    if (g_host_driver) {
        meta_ok &= hmi_tag_register_meta(g_host_driver->tag_metadata, g_host_driver->tag_count);
        if (g_host_driver->init) g_host_driver->init();
    }
    meta_ok &= hmi_tag_register_meta(s_system_meta, (uint8_t)(sizeof(s_system_meta) / sizeof(s_system_meta[0])));
    if (!meta_ok) {
        MessageBoxA(NULL, "Tag metadata conflict between drivers (duplicate tag id).", "Industrial HMI", MB_ICONERROR);
        hmi_timer_shutdown();
        return 3;
    }

    hmi_watchdog_init(&g_ui_wd, now_ms);
    InterlockedExchange(&g_hb_ui_ms, (LONG)now_ms);
    InterlockedExchange(&g_hb_fieldbus_ms, (LONG)now_ms);
    g_exe_size = exe_file_size();

    hmi_renderer_init(&g_renderer, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT);

    char title[160];
    snprintf(title, sizeof(title), "Industrial HMI - Modicon M340/M580 - %s",
             g_plc_endpoint[0] ? g_plc_endpoint : "SIMULATOR");
    if (!hal_display_init(&g_display, hInstance, HMI_DISPLAY_WIDTH_DEFAULT, HMI_DISPLAY_HEIGHT_DEFAULT, title, WndProc)) {
        hmi_timer_shutdown();
        return 1;
    }

    if (g_log_enabled && !hmi_alarm_log_open(g_log_path, HMI_ALARM_LOG_MAX_BYTES)) {
        ui_toast("Alarm log file could not be opened", now_ms);
    }

    HANDLE fb_thread = CreateThread(NULL, 0, FieldbusThreadProc, NULL, 0, NULL);
    if (!fb_thread) {
        MessageBoxA(NULL, "Could not start the fieldbus thread.", "Industrial HMI", MB_ICONERROR);
        hmi_alarm_log_close();
        hmi_timer_shutdown();
        return 2;
    }
    SetThreadPriority(fb_thread, THREAD_PRIORITY_TIME_CRITICAL);

    hal_display_trim_memory();

    uint32_t start_ms = hmi_timer_get_ms();
    uint32_t last_frame_ms = start_ms - HMI_UI_FRAME_PERIOD_MS;
    uint32_t last_beep_ms = start_ms;
    uint32_t last_trim_ms = start_ms;
    uint32_t fps_window_ms = start_ms;
    uint32_t fps_frames = 0;
    MSG msg;

    while (InterlockedCompareExchange(&g_running, 0, 0)) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                InterlockedExchange(&g_running, 0);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!InterlockedCompareExchange(&g_running, 0, 0)) break;

        now_ms = hmi_timer_get_ms();
        uint32_t since = now_ms - last_frame_ms;
        if (since >= HMI_UI_FRAME_PERIOD_MS) {
            if (since > g_ui_max_frame_ms && fps_frames > 2) g_ui_max_frame_ms = since;
            last_frame_ms = now_ms;
            fps_frames++;
            if ((uint32_t)(now_ms - fps_window_ms) >= 1000u) {
                g_ui_frame_hz = fps_frames;
                fps_frames = 0;
                fps_window_ms = now_ms;
            }

            /* Heartbeat + snapshot */
            InterlockedExchange(&g_hb_ui_ms, (LONG)now_ms);
            ui_take_snapshot();

            /* Supervise the fieldbus loop from this (independent) thread */
            hmi_watchdog_ping_ui(&g_ui_wd, now_ms);
            hmi_watchdog_ping_fieldbus(&g_ui_wd, (uint32_t)InterlockedCompareExchange(&g_hb_fieldbus_ms, 0, 0));
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
            if (g_have_snapshot && g_snap.alarms.horn_sounding && (uint32_t)(now_ms - last_beep_ms) >= 1000u) {
                last_beep_ms = now_ms;
                MessageBeep(MB_ICONHAND);
            }

            /* Persistent audit log */
            if (g_have_snapshot) hmi_alarm_log_sync(&g_snap.alarms);

            /* Working-set trim every 10 s */
            if ((uint32_t)(now_ms - last_trim_ms) >= 10000u) {
                last_trim_ms = now_ms;
                hal_display_trim_memory();
            }

            if (g_display.hwnd) {
                InvalidateRect(g_display.hwnd, NULL, FALSE);
                UpdateWindow(g_display.hwnd);
            }

            if (g_exit_after_ms && (uint32_t)(now_ms - start_ms) >= g_exit_after_ms && g_display.hwnd) {
                PostMessageA(g_display.hwnd, WM_CLOSE, 0, 0);
            }
        }

        /* Sleep until the next frame or until input arrives */
        uint32_t elapsed = hmi_timer_get_ms() - last_frame_ms;
        DWORD wait = (elapsed >= HMI_UI_FRAME_PERIOD_MS) ? 0 : (DWORD)(HMI_UI_FRAME_PERIOD_MS - elapsed);
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
    }

    InterlockedExchange(&g_running, 0);
    WaitForSingleObject(fb_thread, 3000);
    CloseHandle(fb_thread);

    /* Flush the journal events produced during shutdown, then close the log. */
    ui_take_snapshot();
    if (g_have_snapshot) hmi_alarm_log_sync(&g_snap.alarms);
    hmi_alarm_log_close();
    write_report();

    hal_display_shutdown(&g_display);
    hmi_timer_shutdown();
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;
    return hmi_run(hInstance, __argc, __argv);
}
