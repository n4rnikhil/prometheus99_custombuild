/**
 * @file device_manager.c
 * @brief Plug-and-play device slots: one non-blocking Modbus TCP client (or the
 *        built-in simulator) per slot, profile-driven I/O, automatic removal
 * @standards Modbus Application Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b
 *
 * Runs entirely inside the 1 kHz fieldbus loop and never blocks:
 *   - connect() is non-blocking and polled without waiting (hal_tcp_connect_poll)
 *   - one request in flight per slot, frames reassembled from partial recv()s
 *   - operator writes go through a per-slot queue and are sent between reads
 *   - a missing reply after HMI_MODBUS_TIMEOUT_MS takes the slot OFFLINE: tags go
 *     COMM_FAULT, the comm-loss alarm is raised, reconnects back off 1 s .. 8 s,
 *     and after remove_after_ms the device leaves the dashboard
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../../include/drivers/device_manager.h"
#include "../../include/drivers/modbus_proto.h"
#include "../../include/hal/timer.h"
#include "../../include/hal/net.h"
#include "../../include/drivers/plc_sim.h"

typedef enum { STEP_NONE, STEP_READ_REGS, STEP_READ_COILS, STEP_WRITE } tcp_step_t;
typedef struct { bool coil; uint16_t addr; int16_t value; } pending_write_t;


typedef struct {
    hmi_device_info_t info;
    hmi_tag_meta_t    meta[HMI_DEVICE_TAG_SPAN];   /* profile meta copies, group = label */
    uint32_t          freed_ms;
    bool              ever_freed;

    /* Modbus TCP */
    hal_sock_t         sock;
    uint32_t           ip;            /* host byte order */
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
    uint16_t           pulse_coil;    /* PULSE_COIL: coil to release ... */
    uint32_t           pulse_due_ms;  /* ... at this time */
    bool               pulse_pending;

#if HMI_FEATURE_SIM
    sim_plc_t          sim;           /* built-in simulated device (--sim) */
    uint32_t           sim_last_ms;
#endif
} slot_t;

static slot_t        s_slots[HMI_MAX_DEVICES];
static bool queue_write(slot_t *s, bool coil, uint16_t addr, int16_t value);
static mb_response_t s_rsp;            /* ~520 bytes, kept off the stack */
static uint32_t      s_remove_after_ms;
static bool          s_net_ok;

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
/* Simulator transport: a plc_sim.c device model, driven with Modbus requests */
/* ========================================================================= */
#if HMI_FEATURE_SIM
static void sim_poll(slot_t *s, hmi_tag_table_t *tags, uint32_t now_ms)
{
    sim_plc_t *m = &s->sim;
    if (s->sim_last_ms == 0) s->sim_last_ms = now_ms;
    uint32_t steps = 0;
    while ((uint32_t)(now_ms - s->sim_last_ms) >= 100u && steps < 50u) { s->sim_last_ms += 100u; sim_step(m); steps++; }
    if ((uint32_t)(now_ms - s->sim_last_ms) >= 100u) s->sim_last_ms = now_ms;
    char ev[SIM_EVENT_LEN];
    while (sim_pop_event(m, ev, sizeof(ev))) {}              /* the journal already has the HMI's commands */

    const hmi_device_profile_t *p = s->info.profile;
    uint8_t slot = s->info.slot;
    for (uint8_t i = 0; i < p->regs.count; i++) {
        uint16_t a = (uint16_t)(p->regs.start + i);
        if (p->regs.rel_tags[i] != HMI_REL_NONE && a < SIM_HR)
            hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, p->regs.rel_tags[i]), m->hr[a], QUALITY_SIMULATED);
    }
    for (uint8_t i = 0; i < p->coils.count; i++) {
        uint16_t a = (uint16_t)(p->coils.start + i);
        if (p->coils.rel_tags[i] != HMI_REL_NONE && a < SIM_COILS)
            hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->coils.rel_tags[i]), m->coils[a] != 0, QUALITY_SIMULATED);
    }
    hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, p->comm_tag), false, QUALITY_GOOD);
    set_state(s, HMI_DEV_ONLINE, now_ms);
}

/* Same Modbus request a real device would get, so the model's write rules apply. */
static bool sim_write(slot_t *s, bool coil, uint16_t addr, int16_t value)
{
    mb_request_t rq = { 1, 1, coil ? MB_FC_WRITE_SINGLE_COIL : MB_FC_WRITE_SINGLE_REG, addr, (uint16_t)value };
    uint8_t adu[16], out[SIM_MAX_ADU];
    size_t n = mb_encode_request(&rq, adu, sizeof(adu));
    size_t r = n ? sim_handle(&s->sim, adu, n, out, sizeof(out)) : 0;
    bool ok = r > 0 && mb_decode_response(&rq, out, r, &s_rsp) == MB_RESP_OK;
    if (ok) s->info.writes_ok++; else s->info.writes_failed++;
    return ok;
}
#endif

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
    hal_tcp_close(s->sock);
    s->sock = HAL_SOCK_INVALID;
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
    hal_sock_t k = hal_tcp_connect_start(s->ip, s->port);
    if (k == HAL_SOCK_INVALID) { s->next_try_ms = now_ms + s->backoff_ms; return; }
    s->sock = k;
    s->connect_start_ms = now_ms;
    s->connecting = true;
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
    if (hal_tcp_send(s->sock, adu, len) != (int)len) return false;
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
    if (!s_net_ok) return;
    if (s->sock == HAL_SOCK_INVALID) {
        if ((int32_t)(now_ms - s->next_try_ms) >= 0) tcp_start_connect(s, now_ms);
        return;
    }
    if (s->connecting) {
        int c = hal_tcp_connect_poll(s->sock);
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
            int n = hal_tcp_recv(s->sock, s->rx + s->rx_len, sizeof(s->rx) - s->rx_len);
            if (n > 0) { s->rx_len += (size_t)n; continue; }
            if (n < 0) { tcp_close(s, tags, al, now_ms); return; }
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
        if (s->step != STEP_NONE || s->sock == HAL_SOCK_INVALID) return;
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
    s->sock = HAL_SOCK_INVALID;
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
    hal_tcp_close(s->sock);
    s->sock = HAL_SOCK_INVALID;
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
    for (int i = 0; i < HMI_MAX_DEVICES; i++) { s_slots[i].sock = HAL_SOCK_INVALID; s_slots[i].info.slot = (uint8_t)i; }
    s_remove_after_ms = remove_after_ms;
    if (!s_net_ok) s_net_ok = hal_net_init();
}

bool hmi_devmgr_attach_simulator(uint8_t sim_type, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
#if HMI_FEATURE_SIM
    const sim_type_info_t *ti = sim_type_info((sim_type_t)sim_type);
    if (!ti || !ti->is_plc) return false;
    int i = free_slot(now_ms);
    if (i < 0) return false;
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, hmi_profile_match(ti->vendor, ti->product), tags, now_ms);
    s->info.transport = HMI_TRANSPORT_SIM;
    snprintf(s->info.endpoint, sizeof(s->info.endpoint), "Simulator");
    snprintf(s->info.vendor, sizeof(s->info.vendor), "%s", ti->vendor);
    snprintf(s->info.product, sizeof(s->info.product), "%s", ti->product);
    sim_init(&s->sim, (sim_type_t)sim_type);
    s->sim_last_ms = 0;
    set_state(s, HMI_DEV_CONNECTING, now_ms);
    LOG(alarms, now_ms, ALARM_SEV_INFO, "%s attached (simulator)", s->info.label);
    return true;
#else
    (void)sim_type; (void)tags; (void)alarms; (void)now_ms;
    return false;
#endif
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
    uint32_t ip;
    if (!hal_ip_parse(c->host, &ip)) return false;

    const hmi_device_profile_t *p = c->identified ? hmi_profile_match(c->id.vendor, c->id.product)
                                                  : hmi_profile_match(NULL, NULL);
    if (!p->auto_attach && !c->explicit_target) return false;     /* only PLCs attach on their own */
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, p, tags, now_ms);
    s->info.transport = HMI_TRANSPORT_TCP;
    s->info.unit_id = c->unit;
    s->ip = ip;
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
        if (s->pulse_pending && (int32_t)(now_ms - s->pulse_due_ms) >= 0) {   /* release a reset button */
            s->pulse_pending = false;
            queue_write(s, true, s->pulse_coil, 0);
        }
#if HMI_FEATURE_SIM
        if (s->info.transport == HMI_TRANSPORT_SIM) { sim_poll(s, tags, now_ms); continue; }
#endif
        tcp_poll(s, tags, alarms, now_ms);
        if (s->info.state == HMI_DEV_OFFLINE && s_remove_after_ms &&
            (uint32_t)(now_ms - s->info.since_ms) >= s_remove_after_ms) {
            remove_slot(s, tags, alarms, now_ms);
        }
    }
}

static bool queue_write(slot_t *s, bool coil, uint16_t addr, int16_t value)
{
#if HMI_FEATURE_SIM
    if (s->info.transport == HMI_TRANSPORT_SIM) return sim_write(s, coil, addr, value);
#endif
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

void hmi_devmgr_action(uint8_t slot, uint8_t action_idx, int16_t value, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    if (slot >= HMI_MAX_DEVICES) return;
    slot_t *s = &s_slots[slot];
    const hmi_device_profile_t *p = s->info.profile;
    if (s->info.state == HMI_DEV_EMPTY || !p || action_idx >= p->n_actions) return;
    const hmi_device_action_t *a = &p->actions[action_idx];
    uint8_t id = HMI_DEV_TAG(slot, a->rel_tag);
    const char *who = s->info.label;
    const hmi_tag_meta_t *m = &s->meta[a->rel_tag];
    char v[24];
    bool ok;

    if (s->info.state != HMI_DEV_ONLINE || !tag_ok(tags, id)) {
        LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %s refused, device offline", who, a->log_name);
        return;
    }
    int16_t cur = hmi_tag_read_value(tags, id);
    bool blocked = a->block_tag != HMI_REL_NONE && hmi_tag_read_value(tags, HMI_DEV_TAG(slot, a->block_tag)) != 0;
    const hmi_tag_meta_t *bm = a->block_tag != HMI_REL_NONE ? &s->meta[a->block_tag] : NULL;

    switch (a->kind) {
        case HMI_ACT_TOGGLE_COIL: {
            bool on = cur != 0;
            if (!on && blocked) {
                LOG(al, now_ms, ALARM_SEV_WARNING, "%s: start blocked by %s", who, bm && bm->name ? bm->name : "interlock");
                return;
            }
            ok = queue_write(s, true, a->address, (int16_t)(on ? 0 : 1));
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s %s", who, a->log_name,
                ok ? (on ? "stop commanded" : "start commanded") : "write failed");
            break;
        }
        case HMI_ACT_TOGGLE_REG: {
            int16_t target = hmi_action_reg_target(a, cur);
            hmi_format_scaled(v, sizeof(v), target, m->decimals);
            ok = queue_write(s, false, a->address, target);
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s %s %s%s", who, a->log_name,
                ok ? "to" : "write failed", v, m->unit ? m->unit : "");
            break;
        }
        case HMI_ACT_COIL_ON:
            if (blocked) {
                LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %s blocked by %s", who, a->log_name, bm && bm->name ? bm->name : "interlock");
                return;
            }
            /* fall through */
        case HMI_ACT_COIL_OFF:
            ok = queue_write(s, true, a->address, (int16_t)(a->kind == HMI_ACT_COIL_ON ? 1 : 0));
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s - %s%s", who, a->log_name, a->label,
                ok ? "" : " FAILED");
            break;
        case HMI_ACT_PULSE_COIL:
            ok = queue_write(s, true, a->address, 1);
            if (ok) { s->pulse_coil = a->address; s->pulse_due_ms = now_ms + HMI_PULSE_MS; s->pulse_pending = true; }
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s - %s%s", who, a->log_name, a->label,
                ok ? "" : " FAILED");
            break;
        case HMI_ACT_SET_REG:
            hmi_format_scaled(v, sizeof(v), value, m->decimals);
            if (value < a->min || value > a->max) {
                LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %s %s refused, out of range", who, a->log_name, v);
                return;
            }
            ok = queue_write(s, false, a->address, value);
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s -> %s%s%s%s", who, a->log_name, v,
                (m->unit && m->unit[0]) ? " " : "", m->unit ? m->unit : "", ok ? "" : " FAILED");
            break;
        case HMI_ACT_SET_ENUM: {
            if (value < 0 || value >= (int16_t)hmi_action_option_count(a)) return;
            char opt[24];
            hmi_action_option_label(a, (uint8_t)value, opt, sizeof(opt));
            ok = queue_write(s, false, a->address, value);
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s -> %s%s", who, a->log_name, opt, ok ? "" : " FAILED");
            break;
        }
        default:
            break;
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
        hal_tcp_close(s_slots[i].sock);
        s_slots[i].sock = HAL_SOCK_INVALID;
    }
    if (s_net_ok) { hal_net_shutdown(); s_net_ok = false; }
}
