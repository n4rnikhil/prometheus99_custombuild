/**
 * @file device_manager.c
 * @brief Plug-and-play device slots: one non-blocking Modbus TCP client per slot,
 *        module-driven I/O, automatic removal
 * @standards Modbus Application Protocol v1.1b3 / Modbus Messaging on TCP/IP v1.0b
 *
 * Runs entirely inside the 1 kHz fieldbus loop and never blocks:
 *   - connect() is non-blocking and polled without waiting (hal_tcp_connect_poll)
 *   - one request in flight per slot, frames reassembled from partial recv()s
 *   - a read cycle walks the module's blocks: the main register block, any further
 *     register blocks, then the coils; each block's tags are updated as it arrives
 *   - operator writes go through a per-slot queue and are sent between reads
 *   - a missing reply after HMI_MODBUS_TIMEOUT_MS takes the slot OFFLINE: tags go
 *     COMM_FAULT, the comm-loss alarm is raised, reconnects back off 1 s .. 8 s,
 *     and after remove_after_ms the device leaves the dashboard
 *
 * The loopback transport runs the same state machine against an in-process Modbus server
 * function instead of a socket (used by the tests to drive the PLC Simulator's models).
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "fieldbus/device_manager.h"
#include "fieldbus/discovery.h"
#include "fieldbus/modbus_proto.h"
#include "hal/timer.h"
#include "hal/net.h"

typedef enum { STEP_NONE, STEP_READ, STEP_WRITE } tcp_step_t;
typedef struct { bool coil; uint16_t addr; int16_t value; } pending_write_t;

typedef struct {
    hmi_device_info_t info;
    hmi_tag_meta_t    meta[HMI_DEVICE_TAG_SPAN];   /* module meta copies, group = label */
    uint32_t          freed_ms;
    bool              ever_freed;

    /* link */
    hal_sock_t         sock;
    hmi_loopback_fn    lb_fn;         /* loopback transport */
    void              *lb_ctx;
    bool               lb_up;
    uint32_t           ip;            /* host byte order */
    char               host[16];
    uint16_t           port;
    uint16_t           tid;
    uint32_t           next_try_ms, backoff_ms, connect_start_ms, last_cycle_ms;
    bool               connecting;
    bool               cycle_due_now;

    /* request in flight */
    tcp_step_t         step;
    uint8_t            read_k;        /* block of the cycle being read (see block_at) */
    uint8_t            first_k;       /* main block of this module */
    mb_request_t       req;
    uint32_t           sent_ms;
    uint64_t           sent_us;
    uint8_t            rx[MB_MAX_ADU * 2];
    size_t             rx_len;
    uint8_t            exc_streak;    /* consecutive cycles whose main block was answered with an exception */

    pending_write_t    wq[HMI_MODBUS_WRITE_QUEUE];
    uint8_t            wq_head, wq_count;
    bool               pulse_pending; /* PULSE kinds: release ... */
    bool               pulse_coil;
    uint16_t           pulse_addr;
    int16_t            pulse_value;
    uint32_t           pulse_due_ms;  /* ... at this time */
    uint32_t           hb_last_ms;    /* last heartbeat write */
    int16_t            hb_count;      /* 1..32767 */
    bool               hb_pending;    /* heartbeat due: own slot, never queued behind / counted as an operator write */
    bool               hb_inflight;   /* the write on the wire is the heartbeat */
} slot_t;

static slot_t        s_slots[HMI_MAX_DEVICES];
static volatile bool s_heartbeat_ok = true;   /* false while the UI is stalled: a frozen screen must not look alive */
static mb_response_t s_rsp;            /* ~520 bytes, kept off the stack */
static uint32_t      s_remove_after_ms;
static bool          s_net_ok;

static bool queue_write(slot_t *s, bool coil, uint16_t addr, int16_t value);

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

static bool is_loopback(const slot_t *s) { return s->info.transport == HMI_TRANSPORT_LOOPBACK; }
static bool linked(const slot_t *s)      { return is_loopback(s) ? s->lb_up : s->sock != HAL_SOCK_INVALID; }

/* ========================================================================= */
/* Read plan: k = 0 main register block, 1..n_more further blocks, n_more+1 coils */
/* ========================================================================= */
static const hmi_block_map_t *block_at(const hmi_device_profile_t *p, uint8_t k, bool *coil)
{
    *coil = false;
    if (k == 0) return &p->regs;
    if (k <= p->n_more_regs) return &p->more_regs[k - 1];
    if (k == p->n_more_regs + 1u) { *coil = true; return &p->coils; }
    return NULL;
}

/* First block at or after k that has something to read; -1 = none (cycle complete) */
static int next_block(const hmi_device_profile_t *p, uint8_t k)
{
    bool coil;
    for (const hmi_block_map_t *b; (b = block_at(p, k, &coil)) != NULL; k++)
        if (b->count) return k;
    return -1;
}

/* Tag value of a FLOAT32 register pair, scaled by the tag's decimals. false = not a number. */
static bool float_value(uint16_t hi, uint16_t lo, uint8_t decimals, int16_t *out)
{
    uint32_t u = ((uint32_t)hi << 16) | lo;
    float f;
    memcpy(&f, &u, sizeof(f));
    if (f != f || f > 3.0e38f || f < -3.0e38f) return false;     /* NaN / infinity: "not available" */
    for (uint8_t d = 0; d < decimals && d < 4; d++) f *= 10.0f;
    f += f >= 0.0f ? 0.5f : -0.5f;
    *out = f >= 32767.0f ? 32767 : f <= -32768.0f ? -32768 : (int16_t)f;
    return true;
}

static void mark_block(slot_t *s, hmi_tag_table_t *tags, const hmi_block_map_t *b, uint8_t q)
{
    uint8_t items = b->format == HMI_FMT_FLOAT32 ? (uint8_t)(b->count / 2u) : b->count;
    for (uint8_t i = 0; i < items; i++)
        if (b->rel_tags[i] != HMI_REL_NONE) hmi_tag_set_quality(tags, HMI_DEV_TAG(s->info.slot, b->rel_tags[i]), q);
}

static void apply_block(slot_t *s, hmi_tag_table_t *tags, const hmi_block_map_t *b, bool coil, const mb_response_t *r)
{
    uint8_t slot = s->info.slot;
    if (coil) {
        for (uint8_t i = 0; i < b->count; i++)
            if (b->rel_tags[i] != HMI_REL_NONE)
                hmi_tag_write_digital(tags, HMI_DEV_TAG(slot, b->rel_tags[i]), mb_coil_get(r, i), QUALITY_GOOD);
        return;
    }
    if (b->format == HMI_FMT_FLOAT32) {
        for (uint8_t i = 0; i < b->count / 2u && 2u * i + 1u < r->count; i++) {
            uint8_t rel = b->rel_tags[i];
            if (rel == HMI_REL_NONE) continue;
            int16_t v;
            if (float_value(r->regs[2u * i], r->regs[2u * i + 1u], s->meta[rel].decimals, &v))
                hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, rel), v, QUALITY_GOOD);
            else
                hmi_tag_set_quality(tags, HMI_DEV_TAG(slot, rel), QUALITY_OUT_OF_RANGE);
        }
        return;
    }
    for (uint8_t i = 0; i < b->count && i < r->count; i++)
        if (b->rel_tags[i] != HMI_REL_NONE)
            hmi_tag_write_analog(tags, HMI_DEV_TAG(slot, b->rel_tags[i]), (int16_t)r->regs[i], QUALITY_GOOD);
}

/* Status-word bits -> digital tags */
static void apply_bits(slot_t *s, hmi_tag_table_t *tags)
{
    const hmi_device_profile_t *p = s->info.profile;
    for (uint8_t i = 0; i < p->n_bits; i++) {
        const hmi_bit_map_t *m = &p->bits[i];
        uint8_t src = HMI_DEV_TAG(s->info.slot, m->src_rel), dst = HMI_DEV_TAG(s->info.slot, m->dst_rel);
        if (hmi_tag_read_quality(tags, src) != QUALITY_GOOD) { hmi_tag_set_quality(tags, dst, QUALITY_COMM_FAULT); continue; }
        uint16_t w = (uint16_t)hmi_tag_read_value(tags, src);
        hmi_tag_write_digital(tags, dst, (w & m->mask) == m->match, QUALITY_GOOD);
    }
}

/* ========================================================================= */
/* Link                                                                       */
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

static void link_close(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    hal_tcp_close(s->sock);
    s->sock = HAL_SOCK_INVALID;
    s->lb_up = false;
    s->connecting = false;
    s->step = STEP_NONE;
    s->hb_pending = false;
    s->hb_inflight = false;
    s->rx_len = 0;
    s->next_try_ms = now_ms + s->backoff_ms;
    s->backoff_ms *= 2u;
    if (s->backoff_ms > HMI_MODBUS_BACKOFF_MAX_MS) s->backoff_ms = HMI_MODBUS_BACKOFF_MAX_MS;
    s->info.writes_failed += s->wq_count;       /* writes queued against a state we can no longer see */
    s->wq_count = 0;
    go_offline(s, tags, al, now_ms);
}

/* Send one ADU: over the socket, or straight into the loopback server (its reply lands in rx). */
static bool link_send(slot_t *s, const uint8_t *adu, size_t len)
{
    if (!is_loopback(s)) return hal_tcp_send(s->sock, adu, len) == (int)len;
    uint8_t out[MB_MAX_ADU];
    size_t n = s->lb_fn ? s->lb_fn(s->lb_ctx, adu, len, out, sizeof(out)) : 0;   /* 0 = no reply (frozen) */
    if (n > sizeof(s->rx) - s->rx_len) return false;
    memcpy(s->rx + s->rx_len, out, n);
    s->rx_len += n;
    return true;
}

static bool send_request(slot_t *s, tcp_step_t step, uint8_t fc, uint16_t addr, uint16_t qv, uint32_t now_ms)
{
    uint8_t adu[MB_MAX_ADU];
    s->req.tid = ++s->tid;
    s->req.unit = s->info.unit_id;
    s->req.fc = fc;
    s->req.addr = addr;
    s->req.qty_or_value = qv;
    size_t len = mb_encode_request(&s->req, adu, sizeof(adu));
    if (len == 0) return false;
    s->info.requests++;
    s->step = step;
    s->sent_ms = now_ms;
    s->sent_us = hmi_timer_get_us();
    if (!link_send(s, adu, len)) { s->step = STEP_NONE; return false; }
    return true;
}

static bool send_read(slot_t *s, uint8_t k, uint32_t now_ms)
{
    bool coil;
    const hmi_block_map_t *b = block_at(s->info.profile, k, &coil);
    s->read_k = k;
    return send_request(s, STEP_READ, coil ? MB_FC_READ_COILS : MB_FC_READ_HOLDING, b->start, b->count, now_ms);
}

static void cycle_done(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    apply_bits(s, tags);
    hmi_tag_write_digital(tags, HMI_DEV_TAG(s->info.slot, s->info.profile->comm_tag), false, QUALITY_GOOD);
    s->exc_streak = 0;
    if (s->info.state == HMI_DEV_OFFLINE) LOG(al, now_ms, ALARM_SEV_INFO, "%s back online", s->info.label);
    else if (s->info.state == HMI_DEV_CONNECTING) LOG(al, now_ms, ALARM_SEV_INFO, "%s online", s->info.label);
    set_state(s, HMI_DEV_ONLINE, now_ms);
}

/* Move on to the next block of the cycle, or finish it. false = drop the link */
static bool continue_cycle(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    int k = next_block(s->info.profile, (uint8_t)(s->read_k + 1u));
    if (k >= 0) return send_read(s, (uint8_t)k, now_ms);
    cycle_done(s, tags, al, now_ms);
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
        if (step == STEP_WRITE) {
            if (s->hb_inflight) s->hb_inflight = false;      /* a refused heartbeat is not an operator write */
            else s->info.writes_failed++;
            return true;
        }
        if (s->read_k != s->first_k) {                     /* optional block: only its tags go bad */
            bool coil;
            mark_block(s, tags, block_at(s->info.profile, s->read_k, &coil), QUALITY_COMM_FAULT);
            return continue_cycle(s, tags, al, now_ms);
        }
        mark_down(s, tags);                                /* main map does not fit this device */
        /* A device that keeps answering with exceptions delivers no valid data: it is as good as
         * lost, so raise the comm-loss alarm instead of showing ONLINE/CONNECTING with dead tags.
         * The link stays open; the first complete good cycle brings it back (cycle_done). */
        if (s->exc_streak < HMI_MODBUS_EXC_OFFLINE) s->exc_streak++;
        if (s->exc_streak >= HMI_MODBUS_EXC_OFFLINE) go_offline(s, tags, al, now_ms);
        return true;
    }
    if (st != MB_RESP_OK) { s->info.protocol_errors++; return false; }

    s->info.responses++;
    s->backoff_ms = HMI_MODBUS_BACKOFF_MIN_MS;
    if (step == STEP_WRITE) {
        if (s->hb_inflight) { s->hb_inflight = false; return true; }
        s->info.writes_ok++;
        s->cycle_due_now = true;
        return true;
    }
    if (step == STEP_READ) {
        bool coil;
        const hmi_block_map_t *b = block_at(s->info.profile, s->read_k, &coil);
        apply_block(s, tags, b, coil, &s_rsp);
        return continue_cycle(s, tags, al, now_ms);
    }
    return true;
}

static void link_poll(slot_t *s, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    bool lb = is_loopback(s);
    if (!lb && !s_net_ok) return;
    if (!linked(s)) {
        if ((int32_t)(now_ms - s->next_try_ms) < 0) return;
        if (lb) {
            s->lb_up = true;
            s->info.connects++;
            s->cycle_due_now = true;
        } else {
            hal_sock_t k = hal_tcp_connect_start(s->ip, s->port);
            if (k == HAL_SOCK_INVALID) { s->next_try_ms = now_ms + s->backoff_ms; return; }
            s->sock = k;
            s->connect_start_ms = now_ms;
            s->connecting = true;
            return;
        }
    }
    if (s->connecting) {
        int c = hal_tcp_connect_poll(s->sock);
        if (c < 0 || (uint32_t)(now_ms - s->connect_start_ms) > HMI_MODBUS_CONNECT_TIMEOUT_MS) {
            link_close(s, tags, al, now_ms);
            return;
        }
        if (c == 0) return;
        s->connecting = false;
        s->info.connects++;
        s->cycle_due_now = true;
    }

    if (s->step != STEP_NONE) {
        while (!lb) {
            if (s->rx_len >= sizeof(s->rx)) { s->info.protocol_errors++; link_close(s, tags, al, now_ms); return; }
            int n = hal_tcp_recv(s->sock, s->rx + s->rx_len, sizeof(s->rx) - s->rx_len);
            if (n > 0) { s->rx_len += (size_t)n; continue; }
            if (n < 0) { link_close(s, tags, al, now_ms); return; }
            break;
        }
        while (s->step != STEP_NONE && s->rx_len > 0) {
            int32_t need = mb_adu_length(s->rx, s->rx_len);
            if (need < 0 || (size_t)need > MB_MAX_ADU) { s->info.protocol_errors++; link_close(s, tags, al, now_ms); return; }
            if (need == 0 || (size_t)need > s->rx_len) break;
            /* consume the frame BEFORE acting on it: handling may send the next request */
            static uint8_t frame[MB_MAX_ADU];
            size_t flen = (size_t)need;
            memcpy(frame, s->rx, flen);
            memmove(s->rx, s->rx + flen, s->rx_len - flen);
            s->rx_len -= flen;
            if (!on_frame(s, tags, al, frame, flen, now_ms)) { link_close(s, tags, al, now_ms); return; }
        }
        if (s->step != STEP_NONE && (uint32_t)(now_ms - s->sent_ms) > HMI_MODBUS_TIMEOUT_MS) {
            s->info.timeouts++;
            if (s->step == STEP_WRITE && !s->hb_inflight) s->info.writes_failed++;
            link_close(s, tags, al, now_ms);
        }
        if (s->step != STEP_NONE || !linked(s)) return;
    }

    if (s->wq_count) {
        pending_write_t *w = &s->wq[s->wq_head];
        s->wq_head = (uint8_t)((s->wq_head + 1u) % HMI_MODBUS_WRITE_QUEUE);
        s->wq_count--;
        bool ok = w->coil ? send_request(s, STEP_WRITE, MB_FC_WRITE_SINGLE_COIL, w->addr, w->value ? 1u : 0u, now_ms)
                          : send_request(s, STEP_WRITE, MB_FC_WRITE_SINGLE_REG, w->addr, (uint16_t)w->value, now_ms);
        if (!ok) { s->info.writes_failed++; link_close(s, tags, al, now_ms); }
        return;
    }
    if (s->hb_pending && s->info.profile) {               /* operator writes first, then the heartbeat */
        s->hb_pending = false;
        s->hb_inflight = true;
        if (!send_request(s, STEP_WRITE, MB_FC_WRITE_SINGLE_REG, s->info.profile->heartbeat_reg,
                          (uint16_t)s->hb_count, now_ms)) link_close(s, tags, al, now_ms);
        return;
    }
    if (s->cycle_due_now || (uint32_t)(now_ms - s->last_cycle_ms) >= HMI_MODBUS_POLL_MS) {
        s->cycle_due_now = false;
        s->last_cycle_ms = now_ms;
        int k = next_block(s->info.profile, 0);
        if (k < 0) { cycle_done(s, tags, al, now_ms); return; }
        if (!send_read(s, (uint8_t)k, now_ms)) link_close(s, tags, al, now_ms);
    }
}

/* ========================================================================= */
/* Slots                                                                      */
/* ========================================================================= */
/* Devices found on the network that could not be attached because every place is in use (or kept
 * for a --plc device). STOP ALL cannot reach them: each one is named in the log, and STOP ALL counts
 * it as NOT stopped. Discovery reports them again every sweep; one not seen for a while is dropped. */
#define OVERFLOW_MAX       16
#define OVERFLOW_FORGET_MS 15000u
#define OVERFLOW_RELOG_MS  300000u
typedef struct { bool used; char host[16]; uint16_t port; uint8_t unit; uint32_t seen_ms, logged_ms; } overflow_t;
static overflow_t s_over[OVERFLOW_MAX];

static void overflow_note(const hmi_device_candidate_t *c, hmi_alarm_supervisor_t *alarms, uint32_t now_ms,
                          const char *why)
{
    overflow_t *e = NULL, *spare = NULL;
    for (int i = 0; i < OVERFLOW_MAX; i++) {
        overflow_t *o = &s_over[i];
        if (o->used && o->port == c->port && o->unit == c->unit && strcmp(o->host, c->host) == 0) { e = o; break; }
        if (!o->used || (uint32_t)(now_ms - o->seen_ms) >= OVERFLOW_FORGET_MS) { if (!spare) spare = o; }
    }
    if (!e) {
        if (!spare) return;                  /* more than OVERFLOW_MAX: the count in the log still says so */
        e = spare;
        memset(e, 0, sizeof(*e));
        e->used = true;
        snprintf(e->host, sizeof(e->host), "%s", c->host);
        e->port = c->port;
        e->unit = c->unit;
    }
    e->seen_ms = now_ms;
    if (e->logged_ms == 0 || (uint32_t)(now_ms - e->logged_ms) >= OVERFLOW_RELOG_MS) {
        e->logged_ms = now_ms ? now_ms : 1;
        LOG(alarms, now_ms, ALARM_SEV_WARNING, "%s:%u NOT shown: %s", c->host, (unsigned)c->port, why);
    }
}

static void overflow_forget(const char *host, uint16_t port, uint8_t unit)
{
    for (int i = 0; i < OVERFLOW_MAX; i++) {
        overflow_t *o = &s_over[i];
        if (o->used && o->port == port && o->unit == unit && strcmp(o->host, host) == 0) o->used = false;
    }
}

uint8_t hmi_devmgr_unattached(uint32_t now_ms)
{
    uint8_t n = 0;
    for (int i = 0; i < OVERFLOW_MAX; i++)
        if (s_over[i].used && (uint32_t)(now_ms - s_over[i].seen_ms) < OVERFLOW_FORGET_MS) n++;
    return n;
}

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

/* Lowest instance number not used by another present device of the same module */
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

/* Bind the module into the slot's tag block (slot must be EMPTY: the UI does not read it). */
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
    int k = next_block(p, 0);
    s->first_k = (uint8_t)(k < 0 ? 0 : k);
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
    s->lb_up = false;
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

static void copy_identity(slot_t *s, const char *vendor, const char *product)
{
    snprintf(s->info.vendor, sizeof(s->info.vendor), "%s", vendor ? vendor : "");
    snprintf(s->info.product, sizeof(s->info.product), "%s", product ? product : "");
}

/* ========================================================================= */
/* API                                                                        */
/* ========================================================================= */
void hmi_devmgr_init(uint32_t remove_after_ms)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_over, 0, sizeof(s_over));
    for (int i = 0; i < HMI_MAX_DEVICES; i++) { s_slots[i].sock = HAL_SOCK_INVALID; s_slots[i].info.slot = (uint8_t)i; }
    s_remove_after_ms = remove_after_ms;
    if (!s_net_ok) s_net_ok = hal_net_init();
}

bool hmi_devmgr_attach_loopback(const char *vendor, const char *product, const char *app,
                                const char *driver_key, hmi_loopback_fn fn, void *ctx,
                                hmi_tag_table_t *tags, hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    if (!fn) return false;
    const hmi_device_profile_t *p = driver_key ? hmi_profile_by_key(driver_key) : hmi_profile_match(vendor, product, app);
    if (!p || (!driver_key && !p->auto_attach)) return false;   /* same rule as discovery: PLCs only */
    int i = free_slot(now_ms);
    if (i < 0) return false;
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, p, tags, now_ms);
    s->info.transport = HMI_TRANSPORT_LOOPBACK;
    s->info.unit_id = 1;
    s->lb_fn = fn;
    s->lb_ctx = ctx;
    s->next_try_ms = now_ms;
    snprintf(s->info.endpoint, sizeof(s->info.endpoint), "Loopback");
    copy_identity(s, vendor, product);
    set_state(s, HMI_DEV_CONNECTING, now_ms);
    LOG(alarms, now_ms, ALARM_SEV_INFO, "%s attached (loopback)", s->info.label);
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

/* The module a candidate gets: forced with @key, else by identity (FC43), else generic. */
const hmi_device_profile_t *hmi_devmgr_module_for(const hmi_device_candidate_t *c)
{
    if (c->driver[0]) {
        const hmi_device_profile_t *f = hmi_profile_by_key(c->driver);
        if (f) return f;
    }
    return c->identified ? hmi_profile_match(c->id.vendor, c->id.product, c->id.app)
                         : hmi_profile_match(NULL, NULL, NULL);
}

/* Places still kept for --plc devices that are not attached yet (other than c itself) */
static int reserved_slots(const hmi_device_candidate_t *c)
{
    int n = 0;
    for (uint8_t i = 0; i < hmi_discovery_explicit_count(); i++) {
        char host[16]; uint16_t port; uint8_t unit;
        if (!hmi_discovery_explicit_at(i, host, &port, &unit)) continue;
        if (port == c->port && unit == c->unit && strcmp(host, c->host) == 0) continue;
        if (!hmi_devmgr_is_attached(host, port, unit)) n++;
    }
    return n;
}

static int free_slot_count(uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < HMI_MAX_DEVICES; i++) {
        const slot_t *s = &s_slots[i];
        if (s->info.state == HMI_DEV_EMPTY &&
            !(s->ever_freed && (uint32_t)(now_ms - s->freed_ms) < HMI_DEVICE_SLOT_COOLDOWN_MS)) n++;
    }
    return n;
}

bool hmi_devmgr_attach(const hmi_device_candidate_t *c, hmi_tag_table_t *tags,
                       hmi_alarm_supervisor_t *alarms, uint32_t now_ms)
{
    if (!c || hmi_devmgr_is_attached(c->host, c->port, c->unit)) return false;
    const hmi_device_profile_t *p = hmi_devmgr_module_for(c);
    if (!p->auto_attach && !c->explicit_target) return false;     /* only PLCs attach on their own */
    int i = free_slot(now_ms);
    if (i < 0) {
        char why[40];
        snprintf(why, sizeof(why), "all %d places in use", HMI_MAX_DEVICES);
        overflow_note(c, alarms, now_ms, why);
        return false;
    }
    if (!c->explicit_target && free_slot_count(now_ms) <= reserved_slots(c)) {
        overflow_note(c, alarms, now_ms, "place kept for a --plc device");
        return false;
    }
    uint32_t ip;
    if (!hal_ip_parse(c->host, &ip)) return false;
    overflow_forget(c->host, c->port, c->unit);
    slot_t *s = &s_slots[i];
    bind_slot(s, (uint8_t)i, p, tags, now_ms);
    s->info.transport = HMI_TRANSPORT_TCP;
    s->info.unit_id = c->unit;
    s->ip = ip;
    snprintf(s->host, sizeof(s->host), "%s", c->host);
    s->port = c->port;
    snprintf(s->info.endpoint, sizeof(s->info.endpoint), "%s:%u", c->host, (unsigned)c->port);
    if (c->identified) copy_identity(s, c->id.vendor, c->id.product);
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
        /* Release a reset button. Kept pending while the queue is full or the link is down, and sent
         * as soon as it can be: a lost release would leave the PLC's reset input on. */
        if (s->pulse_pending && (int32_t)(now_ms - s->pulse_due_ms) >= 0 &&
            s->info.state == HMI_DEV_ONLINE && s->wq_count < HMI_MODBUS_WRITE_QUEUE &&
            queue_write(s, s->pulse_coil, s->pulse_addr, s->pulse_value))
            s->pulse_pending = false;
        link_poll(s, tags, alarms, now_ms);
        const hmi_device_profile_t *pf = s->info.profile;
        if (pf && pf->has_heartbeat && s_heartbeat_ok && s->info.state == HMI_DEV_ONLINE &&
            (uint32_t)(now_ms - s->hb_last_ms) >= HMI_HEARTBEAT_MS) {
            s->hb_last_ms = now_ms;
            s->hb_count = (int16_t)(s->hb_count >= 32767 ? 1 : s->hb_count + 1);   /* never 0: 0 = never seen */
            s->hb_pending = true;
        }
        if (s->info.state == HMI_DEV_OFFLINE && s_remove_after_ms &&
            (uint32_t)(now_ms - s->info.since_ms) >= s_remove_after_ms) {
            remove_slot(s, tags, alarms, now_ms);
        }
    }
}

static bool queue_write(slot_t *s, bool coil, uint16_t addr, int16_t value)
{
    if (s->info.state != HMI_DEV_ONLINE || s->wq_count >= HMI_MODBUS_WRITE_QUEUE) { s->info.writes_failed++; return false; }
    pending_write_t *w = &s->wq[(s->wq_head + s->wq_count) % HMI_MODBUS_WRITE_QUEUE];
    w->coil = coil; w->addr = addr; w->value = value;
    s->wq_count++;
    return true;
}

/* A command-word sequence goes into the queue whole or not at all */
static bool queue_seq(slot_t *s, uint16_t addr, const int16_t *seq, uint8_t n)
{
    if (!seq || n == 0 || n > HMI_MAX_SEQ || s->info.state != HMI_DEV_ONLINE ||
        HMI_MODBUS_WRITE_QUEUE - s->wq_count < n) { s->info.writes_failed++; return false; }
    for (uint8_t i = 0; i < n; i++) queue_write(s, false, addr, seq[i]);
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
        case HMI_ACT_REG_ON:
            if (blocked) {
                LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %s blocked by %s", who, a->log_name, bm && bm->name ? bm->name : "interlock");
                return;
            }
            /* fall through */
        case HMI_ACT_COIL_OFF:
        case HMI_ACT_REG_OFF:
            if (a->kind == HMI_ACT_COIL_ON || a->kind == HMI_ACT_COIL_OFF)
                ok = queue_write(s, true, a->address, (int16_t)(a->kind == HMI_ACT_COIL_ON ? 1 : 0));
            else
                ok = queue_seq(s, a->address, a->seq, a->seq_len);
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s - %s%s", who, a->log_name, a->label,
                ok ? "" : " FAILED");
            break;
        case HMI_ACT_PULSE_COIL:
        case HMI_ACT_PULSE_REG: {
            bool coil = a->kind == HMI_ACT_PULSE_COIL;
            if (!coil && (!a->seq || a->seq_len < 2)) return;
            if (s->pulse_pending) {          /* another reset still held: release it first, never drop it */
                if (HMI_MODBUS_WRITE_QUEUE - s->wq_count < 2) { ok = false; s->info.writes_failed++; }
                else { queue_write(s, s->pulse_coil, s->pulse_addr, s->pulse_value); s->pulse_pending = false; }
            }
            if (!s->pulse_pending) ok = queue_write(s, coil, a->address, coil ? 1 : a->seq[0]);
            if (ok) {
                s->pulse_pending = true;
                s->pulse_coil = coil;
                s->pulse_addr = a->address;
                s->pulse_value = coil ? 0 : a->seq[1];
                s->pulse_due_ms = now_ms + HMI_PULSE_MS;
            }
            LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_WARNING, "%s: %s - %s%s", who, a->log_name, a->label,
                ok ? "" : " FAILED");
            break;
        }
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

void hmi_devmgr_heartbeat_enable(bool ok) { s_heartbeat_ok = ok; }

/* A device that can be set in motion from the HMI: it must be stopped by STOP ALL */
static bool can_start(const hmi_device_profile_t *p)
{
    if (p->estop_coil != 0xFFFFu) return true;
    for (uint8_t k = 0; k < p->n_actions; k++) {
        uint8_t kd = p->actions[k].kind;
        if (hmi_act_is_start(kd) || kd == HMI_ACT_TOGGLE_COIL || kd == HMI_ACT_TOGGLE_REG || p->actions[k].stop_all)
            return true;
    }
    return false;
}

/* The software stop of one attached device. Records and returns the outcome (hmi_stop_result_t). */
static uint8_t stop_one(slot_t *s, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    const hmi_device_profile_t *p = s->info.profile;
    if (!can_start(p)) {                                                       /* a meter, a passive device */
        if (p->n_actions)
            LOG(al, now_ms, ALARM_SEV_INFO, "%s: nothing to stop (no start control)", s->info.label);
        return s->info.stop_result = HMI_STOP_NOTHING;
    }
    if (s->info.state != HMI_DEV_ONLINE) {
        LOG(al, now_ms, ALARM_SEV_CRITICAL, "%s: OFFLINE, not stopped - stop it at the machine", s->info.label);
        return s->info.stop_result = HMI_STOP_OFFLINE;
    }
    /* A stop overrides everything queued before it: never let a queued start run after it */
    if (s->wq_count)
        LOG(al, now_ms, ALARM_SEV_WARNING, "%s: %u queued command(s) cancelled by STOP ALL", s->info.label,
            (unsigned)s->wq_count);
    s->wq_count = 0;
    /* ... but a reset pulse that has started must be completed: its release (0) goes first, or the
     * reset / trip-reset input of the PLC would be left on */
    if (s->pulse_pending) {
        s->pulse_pending = false;
        queue_write(s, s->pulse_coil, s->pulse_addr, s->pulse_value);
    }

    bool sent = false, failed = false;
    if (p->estop_coil != 0xFFFFu) {
        if (queue_write(s, true, p->estop_coil, 1)) sent = true; else failed = true;
    } else {
        for (uint8_t k = 0; k < p->n_actions; k++) {
            const hmi_device_action_t *a = &p->actions[k];
            bool ok;
            if (a->kind == HMI_ACT_COIL_OFF)                          ok = queue_write(s, true, a->address, 0);
            else if (a->kind == HMI_ACT_REG_OFF)                      ok = queue_seq(s, a->address, a->seq, a->seq_len);
            else if (a->kind == HMI_ACT_SET_ENUM && a->stop_all)   ok = queue_write(s, false, a->address, 0);
            else continue;
            if (ok) sent = true; else failed = true;
        }
    }
    if (sent && !failed) {
        LOG(al, now_ms, ALARM_SEV_ESTOP, "%s: stop sent", s->info.label);
        return s->info.stop_result = HMI_STOP_SENT;
    }
    LOG(al, now_ms, ALARM_SEV_CRITICAL, "%s: %s - stop it at the machine", s->info.label,
        sent ? "stop only partly sent" : failed ? "stop write FAILED" : "no stop control");
    return s->info.stop_result = (sent || failed) ? HMI_STOP_FAILED : HMI_STOP_NO_CONTROL;
}

uint8_t hmi_devmgr_stop_all(hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms, uint8_t *not_stopped)
{
    (void)tags;
    uint8_t stopped = 0, missed = 0;
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        if (s->info.state == HMI_DEV_EMPTY || !s->info.profile) continue;
        uint8_t r = stop_one(s, al, now_ms);
        if (r == HMI_STOP_SENT) stopped++;
        else if (r != HMI_STOP_NOTHING) missed++;
    }
    /* Machines on the network this HMI could not take on: STOP ALL does not reach them */
    for (int i = 0; i < OVERFLOW_MAX; i++) {
        const overflow_t *o = &s_over[i];
        if (!o->used || (uint32_t)(now_ms - o->seen_ms) >= OVERFLOW_FORGET_MS) continue;
        LOG(al, now_ms, ALARM_SEV_CRITICAL, "%s:%u not on this HMI - stop it at the machine", o->host, (unsigned)o->port);
        missed++;
    }
    if (not_stopped) *not_stopped = missed;
    return stopped;
}

uint8_t hmi_devmgr_stop_pending(hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    uint8_t sent = 0;
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        if (s->info.state != HMI_DEV_ONLINE || !s->info.profile) continue;
        uint8_t r = s->info.stop_result;
        if (r != HMI_STOP_NONE && r != HMI_STOP_OFFLINE) continue;   /* a failed stop is reported, not retried in a loop */
        if (stop_one(s, al, now_ms) == HMI_STOP_SENT) sent++;
    }
    return sent;
}

void hmi_devmgr_stop_clear(void)
{
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) s_slots[i].info.stop_result = HMI_STOP_NONE;
}

uint8_t hmi_devmgr_estop(bool trip, hmi_tag_table_t *tags, hmi_alarm_supervisor_t *al, uint32_t now_ms)
{
    if (trip) return hmi_devmgr_stop_all(tags, al, now_ms, NULL);
    uint8_t sent = 0;
    for (uint8_t i = 0; i < HMI_MAX_DEVICES; i++) {
        slot_t *s = &s_slots[i];
        const hmi_device_profile_t *p = s->info.profile;
        if (s->info.state == HMI_DEV_EMPTY || !p || p->estop_coil == 0xFFFFu) continue;
        uint8_t et = HMI_DEV_TAG(i, p->estop_tag);
        uint8_t st = al->points[et].state;
        if (hmi_tag_read_value(tags, et) == 0) continue;                       /* not tripped */
        if (st == ALARM_PT_UNACK || st == ALARM_PT_RTN_UNACK) {
            LOG(al, now_ms, ALARM_SEV_WARNING, "%s: stop-latch reset refused, ack first", s->info.label);
            continue;
        }
        bool ok = queue_write(s, true, p->estop_coil, 0);
        LOG(al, now_ms, ok ? ALARM_SEV_INFO : ALARM_SEV_CRITICAL, "%s: stop-latch reset%s", s->info.label,
            ok ? " commanded" : " write FAILED");
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
        s_slots[i].lb_up = false;
    }
    if (s_net_ok) { hal_net_shutdown(); s_net_ok = false; }
}
