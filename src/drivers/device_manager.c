/**
 * @file device_manager.c
 * @brief Plug-and-play device slots: one non-blocking Modbus TCP client (or the
 *        built-in simulator) per slot, profile-driven I/O, automatic removal
 * @standards Modbus Application Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b
 *
 * Runs entirely inside the 1 kHz fieldbus loop and never blocks:
 *   - connect() is non-blocking and checked with a zero-timeout select()
 *   - one request in flight per slot, frames reassembled from partial recv()s
 *   - operator writes go through a per-slot queue and are sent between reads
 *   - a missing reply after HMI_MODBUS_TIMEOUT_MS takes the slot OFFLINE: tags go
 *     COMM_FAULT, the comm-loss alarm is raised, reconnects back off 1 s .. 8 s,
 *     and after remove_after_ms the device leaves the dashboard
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../../include/drivers/device_manager.h"
#include "../../include/drivers/modbus_proto.h"
#include "../../include/hal/timer.h"

typedef enum { STEP_NONE, STEP_READ_REGS, STEP_READ_COILS, STEP_WRITE } tcp_step_t;
typedef struct { bool coil; uint16_t addr; int16_t value; } pending_write_t;

typedef struct {
    int32_t  level_x100, rpm, valve, temp_x10, press;
    bool     pump_a, pump_b, estop;
    uint32_t last_ms, rng;
} sim_state_t;

typedef struct {
    hmi_device_info_t info;
    hmi_tag_meta_t    meta[HMI_DEVICE_TAG_SPAN];   /* profile meta copies, group = label */
    uint32_t          freed_ms;
    bool              ever_freed;

    /* Modbus TCP */
    SOCKET             sock;
    struct sockaddr_in addr;
    char               host[16];
    uint16_t           port;
    uint16_t           tid;
    uint32_t           next_try_ms, backoff_ms, connect_start_ms, last_cycle_ms;
    bool               connecting;
    bool               cycle_due_now;
    tcp_step_t         step;
    mb_request_t       req;
    uint32_t           sent_ms;
    uint64_t           sent_us;
    uint8_t            rx[MB_MAX_ADU * 2];
    size_t             rx_len;
    uint16_t           regs[MB_MAX_READ_REGS];
    bool               regs_ok;
    uint8_t            exc_streak;    /* consecutive read cycles answered with an exception */
    pending_write_t    wq[HMI_MODBUS_WRITE_QUEUE];
    uint8_t            wq_head, wq_count;

    sim_state_t        sim;
} slot_t;

static slot_t        s_slots[HMI_MAX_DEVICES];
static mb_response_t s_rsp;            /* ~520 bytes, kept off the stack */
static uint32_t      s_remove_after_ms;
static bool          s_wsa_ok;

/* Journal entry; text is cut to the journal's 56 characters */
static void LOG(hmi_alarm_supervisor_t *al, uint32_t now_ms, uint8_t sev, const char *fmt, ...)
{
    char m[57];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    hmi_alarms_log_event(al, now_ms, HMI_ALARM_TAG_SYSTEM, sev, ALARM_PT_NORMAL, m);
}

static void set_state(slot_t *s, uint8_t st, uint32_t now_ms)
{
    if (s->info.state != st) {
        s->info.state = st;
        s->info.since_ms = now_ms;
    }
}

/* ========================================================================= */
/* Simulator transport (tank skid physics)                                    */
/* ========================================================================= */
static void sim_reset(sim_state_t *m)
{
    memset(m, 0, sizeof(*m));
    m->level_x100 = 72000; m->rpm = 1450; m->valve = 65; m->temp_x10 = 560; m->press = 130;
    m->pump_a = true; m->rng = 0x12345678u;
}

static int32_t sim_noise(sim_state_t *m, int32_t a)
{
    m->rng = m->rng * 1664525u + 1013904223u;
    return a <= 0 ? 0 : (int32_t)((m->rng >> 16) % (uint32_t)(2 * a + 1)) - a;
}

static void sim_step(sim_state_t *m)
{
    if (m->estop) { m->pump_a = false; m->pump_b = false; }
    int32_t target_rpm = m->pump_a ? 1450 : 0;
    if (m->rpm < target_rpm) m->rpm += 60;
    if (m->rpm > target_rpm) m->rpm -= m->estop ? 150 : 80;
    if (m->rpm < 0) m->rpm = 0;
    if (m->pump_a && m->rpm >= 1440) m->rpm = 1450 + sim_noise(m, 6);
    int32_t inflow = (60 * m->rpm) / 1450;
    int32_t outflow = (m->valve * 9) / 10;
    if (m->level_x100 < 2000) outflow = (outflow * m->level_x100) / 2000;
    m->level_x100 += inflow - outflow + sim_noise(m, 3);
    if (m->level_x100 < 0) m->level_x100 = 0;
    if (m->level_x100 > 100000) m->level_x100 = 100000;
    int32_t target_p = 20 + (m->rpm * 95) / 1450 + ((100 - m->valve) * 40) / 100;
    m->press += (target_p - m->press) / 4 + sim_noise(m, 1);
    int32_t target_t = 300 + (m->rpm * 290) / 1450 - (m->pump_b ? 140 : 0);
    m->temp_x10 += (target_t - m->temp_x10) / 40;
}

/* The simulator presents the same register image a real tank-skid PLC would. */
static void sim_poll(slot_t *s, hmi_tag_table_t *tags, uint32_t now_ms)
{
    sim_state_t *m = &s->sim;
    if (m->last_ms == 0) m->last_ms = now_ms;
    uint32_t steps = 0;
    while ((uint32_t)(now_ms - m->last_ms) >= 100u && steps < 50u) { m->last_ms += 100u; sim_step(m); steps++; }
    if ((uint32_t)(now_ms - m->last_ms) >= 100u) m->last_ms = now_ms;

    const hmi_device_profile_t *p = s->info.profile;
    uint16_t regs[5] = { (uint16_t)(m->level_x100 / 100), (uint16_t)m->rpm, (uint16_t)m->valve,
                         (uint16_t)m->press, (uint16_t)(m->temp_x10 / 10) };
    bool coils[3] = { m->pump_a, m->pump_b, m->estop };
    uint8_t slot = s->info.slot;
    for (uint8_t i = 0; i < p->regs.count && i < 5; i++) {
        if (p->regs.rel_tags[i] != HMI_REL_NONE)
            hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, p->regs.rel_tags[i]), (int16_t)regs[i], QUALITY_SIMULATED);
    }
    for (uint8_t i = 0; i < p->coils.count && i < 3; i++) {
        if (p->coils.rel_tags[i] != HMI_REL_NONE)
            hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->coils.rel_tags[i]), coils[i], QUALITY_SIMULATED);
    }
    hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->comm_tag), false, QUALITY_GOOD);
    set_state(s, HMI_DEV_ONLINE, now_ms);
}

static bool sim_write(slot_t *s, bool coil, uint16_t addr, int16_t value)
{
    sim_state_t *m = &s->sim;
    bool ok = true;
    if (coil) {
        if (addr == 0)      m->pump_a = value && !m->estop;
        else if (addr == 1) m->pump_b = value && !m->estop;
        else if (addr == 2) m->estop = value != 0;
        else ok = false;
    } else if (addr == 2) {
        m->valve = value < 0 ? 0 : value > 100 ? 100 : value;
    } else {
        ok = false;
    }
    if (ok) s->info.writes_ok++; else s->info.writes_failed++;
    return ok;
}

/* ========================================================================= */
/* Modbus TCP transport                                                       */
/* ========================================================================= */
static void mark_down(slot_t *s, hmi_tag_table_t *tags)
{
    const hmi_device_profile_t *p = s->info.profile;
    uint8_t slot = s->info.slot;
    for (uint8_t i = 0; i < p->meta_count; i++) {
        uint8_t rel = p->meta[i].tag_id;
        if (rel != p->comm_tag) hmi_tag_set_quality(tags, HMI_DEV_TAG(slot, rel), QUALITY_COMM_FAULT);
    }
}

static void go_offline(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    mark_down(s, tags);
    hmi_tag_write_digital(tags, HMI_DEV_TAG(s->info.slot, s->info.profile->comm_tag), true, QUALITY_GOOD);
    if (s->info.state != HMI_DEV_OFFLINE) {
        LOG(al, now_ms, ALARM_SEV_WARNING, "%s offline (%s)", s->info.label, s->info.endpoint);
        set_state(s, HMI_DEV_OFFLINE, now_ms);
    }
}

static void tcp_close(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    if (s->sock != INVALID_SOCKET) closesocket(s->sock);
    s->sock = INVALID_SOCKET;
    s->connecting = false;
    s->step = STEP_NONE;
    s->rx_len = 0;
    s->regs_ok = false;
    s->next_try_ms = now_ms + s->backoff_ms;
    s->backoff_ms *= 2u;
    if (s->backoff_ms > HMI_MODBUS_BACKOFF_MAX_MS) s->backoff_ms = HMI_MODBUS_BACKOFF_MAX_MS;
    s->info.writes_failed += s->wq_count;       /* writes queued against a state we can no longer see */
    s->wq_count = 0;
    go_offline(s, tags, al, now_ms);
}

static void tcp_start_connect(slot_t *s, uint32_t now_ms)
{
    SOCKET k = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (k == INVALID_SOCKET) { s->next_try_ms = now_ms + s->backoff_ms; return; }
    u_long nb = 1;
    ioctlsocket(k, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(k, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    int rc = connect(k, (const struct sockaddr *)&s->addr, sizeof(s->addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(k);
        s->next_try_ms = now_ms + s->backoff_ms;
        return;
    }
    s->sock = k;
    s->connect_start_ms = now_ms;
    s->connecting = true;
}

static int tcp_check_connect(slot_t *s)
{
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s->sock, &w); FD_SET(s->sock, &e);
    struct timeval tv = { 0, 0 };
    int n = select(0, NULL, &w, &e, &tv);
    if (n == SOCKET_ERROR) return -1;
    if (n == 0) return 0;
    if (FD_ISSET(s->sock, &e)) return -1;
    return FD_ISSET(s->sock, &w) ? 1 : 0;
}

static bool tcp_send(slot_t *s, tcp_step_t step, uint8_t fc, uint16_t addr, uint16_t qv, uint32_t now_ms)
{
    uint8_t adu[MB_MAX_ADU];
    s->req.tid = ++s->tid;
    s->req.unit = s->info.unit_id;
    s->req.fc = fc;
    s->req.addr = addr;
    s->req.qty_or_value = qv;
    size_t len = mb_encode_request(&s->req, adu, sizeof(adu));
    if (len == 0) return false;
    if (send(s->sock, (const char *)adu, (int)len, 0) != (int)len) return false;
    s->info.requests++;
    s->step = step;
    s->sent_ms = now_ms;
    s->sent_us = hmi_timer_get_us();
    return true;
}

static void apply_cycle(slot_t *s, hmi_tag_table_t *tags, const mb_response_t *coils,
                        hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    const hmi_device_profile_t *p = s->info.profile;
    uint8_t slot = s->info.slot;
    for (uint8_t i = 0; i < p->regs.count; i++) {
        if (p->regs.rel_tags[i] != HMI_REL_NONE)
            hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, p->regs.rel_tags[i]), (int16_t)s->regs[i], QUALITY_GOOD);
    }
    for (uint8_t i = 0; coils && i < p->coils.count; i++) {
        if (p->coils.rel_tags[i] != HMI_REL_NONE)
            hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->coils.rel_tags[i]), mb_coil_get(coils, i), QUALITY_GOOD);
    }
    hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->comm_tag), false, QUALITY_GOOD);
    s->exc_streak = 0;
    if (s->info.state == HMI_DEV_OFFLINE) LOG(al, now_ms, ALARM_SEV_INFO, "%s back online", s->info.label);
    else if (s->info.state == HMI_DEV_CONNECTING) LOG(al, now_ms, ALARM_SEV_INFO, "%s online", s->info.label);
    set_state(s, HMI_DEV_ONLINE, now_ms);
}

static bool start_read_cycle(slot_t *s, uint32_t now_ms)
{
    const hmi_device_profile_t *p = s->info.profile;
    if (p->regs.count) return tcp_send(s, STEP_READ_REGS, MB_FC_READ_HOLDING, p->regs.start, p->regs.count, now_ms);
    if (p->coils.count) return tcp_send(s, STEP_READ_COILS, MB_FC_READ_COILS, p->coils.start, p->coils.count, now_ms);
    return true;
}

/* false = drop the link */
static bool on_frame(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al,
                     const uint8_t *adu, size_t len, uint32_t now_ms)
{
    mb_status_t st = mb_decode_response(&s->req, adu, len, &s_rsp);
    if (st == MB_ERR_TID) { s->info.protocol_errors++; return true; }      /* stale frame */
    uint32_t rtt = (uint32_t)(hmi_timer_get_us() - s->sent_us);
    s->info.rtt_us = rtt;
    if (rtt > s->info.max_rtt_us) s->info.max_rtt_us = rtt;
    tcp_step_t step = s->step;
    s->step = STEP_NONE;

    if (st == MB_RESP_EXCEPTION) {
        s->info.exceptions++;
        if (step == STEP_WRITE) s->info.writes_failed++;
        else {
            s->regs_ok = false;
            mark_down(s, tags);                            /* map does not fit this device */
            /* A device that keeps answering with exceptions delivers no valid data: it is as good as
             * lost, so raise the comm-loss alarm instead of showing ONLINE/CONNECTING with dead tags.
             * The link stays open; the first complete good cycle brings it back (apply_cycle). */
            if (s->exc_streak < HMI_MODBUS_EXC_OFFLINE) s->exc_streak++;
            if (s->exc_streak >= HMI_MODBUS_EXC_OFFLINE) go_offline(s, tags, al, now_ms);
        }
        return true;
    }
    if (st != MB_RESP_OK) { s->info.protocol_errors++; return false; }

    s->info.responses++;
    s->backoff_ms = HMI_MODBUS_BACKOFF_MIN_MS;
    const hmi_device_profile_t *p = s->info.profile;
    switch (step) {
        case STEP_READ_REGS:
            for (uint16_t i = 0; i < s_rsp.count && i < MB_MAX_READ_REGS; i++) s->regs[i] = s_rsp.regs[i];
            s->regs_ok = true;
            if (p->coils.count) return tcp_send(s, STEP_READ_COILS, MB_FC_READ_COILS, p->coils.start, p->coils.count, now_ms);
            apply_cycle(s, tags, NULL, al, now_ms);
            break;
        case STEP_READ_COILS:
            if (s->regs_ok || !p->regs.count) apply_cycle(s, tags, &s_rsp, al, now_ms);
            s->regs_ok = false;
            break;
        case STEP_WRITE:
            s->info.writes_ok++;
            s->cycle_due_now = true;
            break;
        default: break;
    }
    return true;
}

static void tcp_poll(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    if (!s_wsa_ok) return;
    if (s->sock == INVALID_SOCKET) {
        if ((int32_t)(now_ms - s->next_try_ms) >= 0) tcp_start_connect(s, now_ms);
        return;
    }
    if (s->connecting) {
        int c = tcp_check_connect(s);
        if (c < 0 || (uint32_t)(now_ms - s->connect_start_ms) > HMI_MODBUS_CONNECT_TIMEOUT_MS) {
            tcp_close(s, tags, al, now_ms);
            return;
        }
        if (c == 0) return;
        s->connecting = false;
        s->info.connects++;
        s->cycle_due_now = true;
    }

    if (s->step != STEP_NONE) {
        for (;;) {
            if (s->rx_len >= sizeof(s->rx)) { s->info.protocol_errors++; tcp_close(s, tags, al, now_ms); return; }
            int n = recv(s->sock, (char *)s->rx + s->rx_len, (int)(sizeof(s->rx) - s->rx_len), 0);
            if (n > 0) { s->rx_len += (size_t)n; continue; }
            if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) { tcp_close(s, tags, al, now_ms); return; }
            break;
        }
        while (s->step != STEP_NONE && s->rx_len > 0) {
            int32_t need = mb_adu_length(s->rx, s->rx_len);
            if (need < 0 || (size_t)need > MB_MAX_ADU) { s->info.protocol_errors++; tcp_close(s, tags, al, now_ms); return; }
            if (need == 0 || (size_t)need > s->rx_len) break;
            /* consume the frame BEFORE acting on it: handling may send the next request */
            static uint8_t frame[MB_MAX_ADU];
            size_t flen = (size_t)need;
            memcpy(frame, s->rx, flen);
            memmove(s->rx, s->rx + flen, s->rx_len - flen);
            s->rx_len -= flen;
            if (!on_frame(s, tags, al, frame, flen, now_ms)) { tcp_close(s, tags, al, now_ms); return; }
        }
        if (s->step != STEP_NONE && (uint32_t)(now_ms - s->sent_ms) > HMI_MODBUS_TIMEOUT_MS) {
            s->info.timeouts++;
            if (s->step == STEP_WRITE) s->info.writes_failed++;
            tcp_close(s, tags, al, now_ms);
        }
        if (s->step != STEP_NONE || s->sock == INVALID_SOCKET) return;
    }

    if (s->wq_count) {
        pending_write_t *w = &s->wq[s->wq_head];
        s->wq_head = (uint8_t)((s->wq_head + 1u) % HMI_MODBUS_WRITE_QUEUE);
        s->wq_count--;
        bool ok = w->coil ? tcp_send(s, STEP_WRITE, MB_FC_WRITE_SINGLE_COIL, w->addr, w->value ? 1u : 0u, now_ms)
                          : tcp_send(s, STEP_WRITE, MB_FC_WRITE_SINGLE_REG, w->addr, (uint16_t)w->value, now_ms);
        if (!ok) { s->info.writes_failed++; tcp_close(s, tags, al, now_ms); }
        return;
    }
    if (s->cycle_due_now || (uint32_t)(now_ms - s->last_cycle_ms) >= HMI_MODBUS_POLL_MS) {
        s->cycle_due_now = false;
        s->last_cycle_ms = now_ms;
        if (!start_read_cycle(s, now_ms)) tcp_close(s, tags, al, now_ms);
    }
}

/* ========================================================================= */
/* Slots                                                                      */
/* ========================================================================= */
static int free_slot(uint32_t now_ms)
{
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        if (s->info.state != HMI_DEV_EMPTY) continue;
        if (s->ever_freed && (uint32_t)(now_ms - s->freed_ms) < HMI_DEVICE_SLOT_COOLDOWN_MS) continue;
        return i;
    }
    return -1;
}

/* Lowest instance number not used by another present device of the same profile */
static unsigned next_instance(const hmi_device_profile_t *p)
{
    char want[24];
    for (unsigned n = 1; n < 100; n++) {
        bool used = false;
        snprintf(want, sizeof(want), "%s %u", p->name, n);
        for (int i = 0; i < HMI_MAX_DEVICES && !used; i++) {
            const slot_t *o = &s_slots[i];
            if (o->info.state != HMI_DEV_EMPTY && strcmp(o->info.label, want) == 0) used = true;
        }
        if (!used) return n;
    }
    return 99;
}

/* Bind the profile into the slot's tag block (slot must be EMPTY: the UI does not read it). */
static void bind_slot(slot_t *s, uint8_t slot, const hmi_device_profile_t *p, hmi_tag_table_t *tags, uint32_t now_ms)
{
    unsigned inst = next_instance(p);
    uint8_t keep_freed = s->ever_freed;
    uint32_t freed = s->freed_ms;
    memset(s, 0, sizeof(*s));
    s->ever_freed = keep_freed;
    s->freed_ms = freed;
    s->sock = INVALID_SOCKET;
    s->backoff_ms = HMI_MODBUS_BACKOFF_MIN_MS;
    s->info.slot = slot;
    s->info.profile = p;
    snprintf(s->info.label, sizeof(s->info.label), "%.18s %u", p->name, inst);
    for (uint8_t i = 0; i < p->meta_count && i < HMI_DEVICE_TAG_SPAN; i++) {
        uint8_t rel = p->meta[i].tag_id;
        if (rel >= HMI_DEVICE_TAG_SPAN) continue;
        s->meta[rel] = p->meta[i];
        s->meta[rel].tag_id = HMI_DEV_TAG(slot, rel);
        s->meta[rel].group = s->info.label;
        hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, rel), 0, QUALITY_COMM_FAULT);
        hmi_tag_bind_meta(HMI_DEV_TAG(slot, rel), &s->meta[rel]);   /* release-ordered publish */
    }
    hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->comm_tag), false, QUALITY_GOOD);
    hmi_tags_clear_trend(tags, slot);
    s->info.since_ms = now_ms;
}

static void remove_slot(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    uint8_t slot = s->info.slot;
    if (s->sock != INVALID_SOCKET) closesocket(s->sock);
    s->sock = INVALID_SOCKET;
    for (uint8_t rel = 0; rel < HMI_DEVICE_TAG_SPAN; rel++) {
        hmi_tag_bind_meta(HMI_DEV_TAG(slot, rel), NULL);
        hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, rel), 0, QUALITY_COMM_FAULT);
    }
    hmi_alarms_clear_range(al, HMI_DEV_TAG(slot, 0), HMI_DEVICE_TAG_SPAN);
    hmi_tags_clear_trend(tags, slot);
    LOG(al, now_ms, ALARM_SEV_WARNING, "%s removed (offline %lus)", s->info.label,
        (unsigned long)((now_ms - s->info.since_ms) / 1000u));
    s->info.state = HMI_DEV_EMPTY;
    s->info.profile = NULL;
    s->ever_freed = true;
    s->freed_ms = now_ms;
}

/* ========================================================================= */
/* API                                                                        */
/* ========================================================================= */
void hmi_devmgr_init(uint32_t remove_after_ms)
{
    memset(s_slots, 0, sizeof(s_slots));
    for (int i = 0; i < HMI_MAX_DEVICES; i++) { s_slots[i].sock = INVALID_SOCKET; s_slots[i].info.slot = (uint8_t)i; }
    s_remove_after_ms = remove_after_ms;
    if (!s_wsa_ok) {
        WSADATA wd;
        s_wsa_ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    }
}

bool hmi_devmgr_attach_simulator(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    int i = free_slot(now_ms);
    if (i < 0) return false;
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, &g_profile_tank_skid, tags, now_ms);
    s->info.transport = HMI_TRANSPORT_SIM;
    snprintf(s->info.endpoint, sizeof(s->info.endpoint), "Simulator");
    snprintf(s->info.vendor, sizeof(s->info.vendor), "Prometheus");
    snprintf(s->info.product, sizeof(s->info.product), "Tank skid simulator");
    sim_reset(&s->sim);
    set_state(s, HMI_DEV_CONNECTING, now_ms);
    LOG(alarms, now_ms, ALARM_SEV_INFO, "%s attached (simulator)", s->info.label);
    return true;
}

bool hmi_devmgr_is_attached(const char *host, uint16_t port, uint8_t unit)
{
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        const slot_t *s = &s_slots[i];
        if (s->info.state != HMI_DEV_EMPTY && s->info.transport == HMI_TRANSPORT_TCP &&
            s->port == port && s->info.unit_id == unit && strcmp(s->host, host) == 0) return true;
    }
    return false;
}

bool hmi_devmgr_attach(const hmi_device_candidate_t *c, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    if (!c || hmi_devmgr_is_attached(c->host, c->port, c->unit)) return false;
    int i = free_slot(now_ms);
    if (i < 0) {
        static uint32_t last_full_log;
        if ((uint32_t)(now_ms - last_full_log) > 30000u || last_full_log == 0) {
            last_full_log = now_ms ? now_ms : 1;
            LOG(alarms, now_ms, ALARM_SEV_WARNING, "Device %s:%u ignored: all %d slots used", c->host,
                (unsigned)c->port, HMI_MAX_DEVICES);
        }
        return false;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(c->port);
    if (inet_pton(AF_INET, c->host, &a.sin_addr) != 1) return false;

    const hmi_device_profile_t *p = c->identified ? hmi_profile_match(c->id.vendor, c->id.product)
                                                  : hmi_profile_match(NULL, NULL);
    if (!p->auto_attach && !c->explicit_target) return false;     /* only PLCs attach on their own */
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, p, tags, now_ms);
    s->info.transport = HMI_TRANSPORT_TCP;
    s->info.unit_id = c->unit;
    s->addr = a;
    snprintf(s->host, sizeof(s->host), "%s", c->host);
    s->port = c->port;
    snprintf(s->info.endpoint, sizeof(s->info.endpoint), "%s:%u", c->host, (unsigned)c->port);
    if (c->identified) {
        snprintf(s->info.vendor, sizeof(s->info.vendor), "%s", c->id.vendor);
        snprintf(s->info.product, sizeof(s->info.product), "%s", c->id.product);
    }
    s->next_try_ms = now_ms;
    set_state(s, HMI_DEV_CONNECTING, now_ms);
    LOG(alarms, now_ms, ALARM_SEV_INFO, "%s found at %s", s->info.label, s->info.endpoint);
    return true;
}

void hmi_devmgr_poll(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        if (s->info.state == HMI_DEV_EMPTY) continue;
        if (s->info.transport == HMI_TRANSPORT_SIM) { sim_poll(s, tags, now_ms); continue; }
        tcp_poll(s, tags, alarms, now_ms);
        if (s->info.state == HMI_DEV_OFFLINE && s_remove_after_ms &&
            (uint32_t)(now_ms - s->info.since_ms) >= s_remove_after_ms) {
            remove_slot(s, tags, alarms, now_ms);
        }
    }
}

static bool queue_write(slot_t *s, bool coil, uint16_t addr, int16_t value)
{
    if (s->info.transport == HMI_TRANSPORT_SIM) return sim_write(s, coil, addr, value);
    if (s->info.state != HMI_DEV_ONLINE || s->wq_count >= HMI_MODBUS_WRITE_QUEUE) { s->info.writes_failed++; return false; }
    pending_write_t *w = &s->wq[(s->wq_head + s->wq_count) % HMI_MODBUS_WRITE_QUEUE];
    w->coil = coil; w->addr = addr; w->value = value;
    s->wq_count++;
    return true;
}

static bool tag_ok(const hmi_tag_table_t *tags, uint8_t id)
{
    uint8_t q = hmi_tag_read_quality(tags, id);
    return q == QUALITY_GOOD || q == QUALITY_SIMULATED;
}

void hmi_devmgr_action(uint8_t slot, uint8_t action_idx, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    if (slot >= HMI_MAX_DEVICES) return;
    slot_t *s = &s_slots[slot];
    const hmi_device_profile_t *p = s->info.profile;
    if (s->info.state == HMI_DEV_EMPTY || !p || action_idx >= p->n_actions) return;
    const hmi_device_action_t *a = &p->actions[action_idx];
    uint8_t id = HMI_DEV_TAG(slot, a->rel_tag);
    const char *who = s->info.label;

    if (!tag_ok(tags, id)) {
        LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %s blocked, status unknown", who, a->log_name);
        return;
    }
    int16_t cur = hmi_tag_read_value(tags, id);
    if (a->kind == HMI_ACT_TOGGLE_COIL) {
        bool on = cur != 0;
        if (!on && a->block_tag != HMI_REL_NONE && hmi_tag_read_value(tags, HMI_DEV_TAG(slot, a->block_tag)) != 0) {
            const hmi_tag_meta_t *bm = &s->meta[a->block_tag];
            LOG(al, now_ms, ALARM_SEV_WARNING, "%s: start blocked by %s", who, bm->name ? bm->name : "interlock");
            return;
        }
        bool ok = queue_write(s, true, a->address, (int16_t)(on ? 0 : 1));
        LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s %s", who, a->log_name,
            ok ? (on ? "stop commanded" : "start commanded") : "write failed");
    } else if (a->kind == HMI_ACT_TOGGLE_REG) {
        int16_t target = hmi_action_reg_target(a, cur);
        char v[16];
        const hmi_tag_meta_t *m = &s->meta[a->rel_tag];
        hmi_format_scaled(v, sizeof(v), target, m->decimals);
        bool ok = queue_write(s, false, a->address, target);
        LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s %s %s%s", who, a->log_name,
            ok ? "to" : "write failed", v, m->unit ? m->unit : "");
    }
}

uint8_t hmi_devmgr_estop(bool trip, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    uint8_t sent = 0;
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        const hmi_device_profile_t *p = s->info.profile;
        if (s->info.state == HMI_DEV_EMPTY || !p || p->estop_coil == 0xFFFFu) continue;
        if (!trip) {
            uint8_t et = HMI_DEV_TAG(i, p->estop_tag);
            uint8_t st = al->points[et].state;
            if (hmi_tag_read_value(tags, et) == 0) continue;                       /* not tripped */
            if (st == ALARM_PT_UNACK || st == ALARM_PT_RTN_UNACK) {
                LOG(al, now_ms, ALARM_SEV_WARNING, "%s: E-stop reset refused, ack first", s->info.label);
                continue;
            }
        }
        bool ok = queue_write(s, true, p->estop_coil, (int16_t)(trip ? 1 : 0));
        LOG(al, now_ms, ok ? (trip ? ALARM_SEV_ESTOP : ALARM_SEV_INFO) : ALARM_SEV_CRITICAL, "%s: E-stop %s%s",
            s->info.label, trip ? "trip" : "reset", ok ? " commanded" : " write FAILED");
        if (ok) sent++;
    }
    return sent;
}

void hmi_devmgr_publish(hmi_device_info_t out[HMI_MAX_DEVICES], uint32_t now_ms)
{
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        out[i] = s_slots[i].info;
        out[i].remove_in_ms = 0;
        if (out[i].state == HMI_DEV_OFFLINE && s_remove_after_ms) {
            uint32_t el = now_ms - out[i].since_ms;
            out[i].remove_in_ms = el >= s_remove_after_ms ? 0 : s_remove_after_ms - el;
        }
    }
}

uint8_t hmi_devmgr_count(uint8_t state_mask)
{
    uint8_t n = 0;
    for (int i = 0; i < HMI_MAX_DEVICES; i++) if (state_mask & (1u << s_slots[i].info.state)) n++;
    return n;
}

void hmi_devmgr_shutdown(void)
{
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        if (s_slots[i].sock != INVALID_SOCKET) closesocket(s_slots[i].sock);
        s_slots[i].sock = INVALID_SOCKET;
    }
    if (s_wsa_ok) { WSACleanup(); s_wsa_ok = false; }
}
