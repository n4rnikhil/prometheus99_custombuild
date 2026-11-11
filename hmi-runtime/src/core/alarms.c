/**
 * @file alarms.c
 * @brief ISA-18.2 alarm engine: per-point state machine, shelving, out of service, flood and
 *        chatter detection, horn, operator guidance and the audit journal
 * @standards ISA-18.2 / IEC 62682 Management of Alarm Systems
 *
 * Runs on the fieldbus thread only. The UI reads a copy (the snapshot).
 */

#include "core/alarms.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

void hmi_alarms_init(hmi_alarm_supervisor_t *sup)
{
    if (!sup) return;
    memset(sup, 0, sizeof(*sup));
    sup->latch_state = ALARM_LATCH_NORMAL;
    sup->highest_severity = ALARM_SEV_NONE;
}

const char* hmi_alarm_sev_name(uint8_t severity)
{
    switch (severity) {
        case ALARM_SEV_NONE:     return "NONE";
        case ALARM_SEV_INFO:     return "INFO";
        case ALARM_SEV_WARNING:  return "WARN";
        case ALARM_SEV_CRITICAL: return "CRIT";
        case ALARM_SEV_ESTOP:    return "ESTOP";
        default:                 return "UNKN";
    }
}

const char* hmi_alarm_state_name(uint8_t state)
{
    switch (state) {
        case ALARM_PT_NORMAL:    return "NORMAL";
        case ALARM_PT_UNACK:     return "UNACK";
        case ALARM_PT_ACKED:     return "ACKED";
        case ALARM_PT_RTN_UNACK: return "RTN-UNACK";
        default:                 return "UNKN";
    }
}

/* ========================================================================= */
/* Journal                                                                    */
/* ========================================================================= */

void hmi_alarms_log_event(hmi_alarm_supervisor_t *sup, uint32_t now_ms, hmi_tag_id_t tag_id,
                          uint8_t severity, uint8_t state, const char *msg)
{
    if (!sup) return;

    sup->journal_total++;
    uint16_t idx = sup->journal_head;
    hmi_alarm_record_t *rec = &sup->journal[idx];

    rec->timestamp_ms = now_ms;
    rec->seq          = sup->journal_total;
    rec->tag_id       = tag_id;
    rec->severity     = severity;
    rec->state        = state;

    size_t i = 0;
    for (; msg && msg[i] && i + 1 < sizeof(rec->message); i++) rec->message[i] = msg[i];
    rec->message[i] = '\0';

    sup->journal_head = (uint16_t)((idx + 1u) % HMI_ALARM_JOURNAL_SIZE);
    if (sup->journal_count < HMI_ALARM_JOURNAL_SIZE) sup->journal_count++;
}

uint16_t hmi_alarms_journal_count(const hmi_alarm_supervisor_t *sup)
{
    return sup ? sup->journal_count : 0;
}

const hmi_alarm_record_t* hmi_alarms_journal_newest(const hmi_alarm_supervisor_t *sup, uint16_t idx)
{
    if (!sup || idx >= sup->journal_count) return NULL;
    int32_t pos = (int32_t)sup->journal_head - 1 - (int32_t)idx;
    while (pos < 0) pos += HMI_ALARM_JOURNAL_SIZE;
    return &sup->journal[pos % HMI_ALARM_JOURNAL_SIZE];
}

/* Record seq sits at index (seq - 1) % size while it is among the newest journal_count records */
const hmi_alarm_record_t* hmi_alarms_journal_by_seq(const hmi_alarm_supervisor_t *sup, uint32_t seq)
{
    if (!sup || sup->journal_count == 0 || seq == 0 || seq > sup->journal_total) return NULL;
    if (sup->journal_total - seq >= sup->journal_count) return NULL;            /* overwritten */
    const hmi_alarm_record_t *rec = &sup->journal[(seq - 1u) % HMI_ALARM_JOURNAL_SIZE];
    return rec->seq == seq ? rec : NULL;
}

/* Journal text: deliberately cut to the record size */
static void msgf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, cap, fmt, ap);
    va_end(ap);
}

/* "Tank skid 2: Tank 01 level" for device tags, plain name otherwise */
static const char* point_label(const hmi_tag_meta_t *meta)
{
    static char buf[64];
    if (!meta) return "Process value";
    if (!meta->group) return meta->name;
    snprintf(buf, sizeof(buf), "%s: %s", meta->group, meta->name);
    return buf;
}

static void fmt_point(char *out, size_t cap, const hmi_tag_meta_t *meta, int16_t val)
{
    char num[16];
    hmi_format_scaled(num, sizeof(num), val, meta ? meta->decimals : 0);
    if (meta && meta->type == TAG_TYPE_DIGITAL) snprintf(out, cap, "%s", val ? "ON" : "OFF");
    else snprintf(out, cap, "%s %s", num, (meta && meta->unit) ? meta->unit : "");
}

/* ========================================================================= */
/* Acknowledge, silence, clear                                                */
/* ========================================================================= */

void hmi_alarms_clear_range(hmi_alarm_supervisor_t *sup, hmi_tag_id_t first_tag, uint16_t count)
{
    if (!sup) return;
    for (uint32_t i = first_tag; i < (uint32_t)first_tag + count && i < HMI_MAX_TAGS; i++) {
        memset(&sup->points[i], 0, sizeof(sup->points[i]));
    }
    if (sup->current_action.active && sup->current_action.tag_id >= first_tag &&
        sup->current_action.tag_id < (uint32_t)first_tag + count) {
        sup->current_action.active = false;     /* rebuilt by the next evaluate */
    }
}

void hmi_alarms_silence_horn(hmi_alarm_supervisor_t *sup)
{
    if (!sup) return;
    sup->horn_silenced = true;
    sup->horn_sounding = false;
}

/* A point changed outside an evaluation: make sure every loop still reaches it */
static void note_point(hmi_alarm_supervisor_t *sup, hmi_tag_id_t id)
{
    if ((uint32_t)id + 1u > sup->points_in_use) sup->points_in_use = (uint16_t)(id + 1u);
}

/* UNACK -> ACKED, RTN_UNACK -> NORMAL. false if there was nothing to acknowledge. */
static bool ack_point(hmi_alarm_point_t *pt, uint32_t now_ms)
{
    if (pt->state == ALARM_PT_UNACK) {
        pt->state = ALARM_PT_ACKED;
    } else if (pt->state == ALARM_PT_RTN_UNACK) {
        pt->state = ALARM_PT_NORMAL;
        pt->peak_severity = ALARM_SEV_NONE;
    } else {
        return false;
    }
    pt->since_ms = now_ms;
    return true;
}

static bool needs_ack(const hmi_alarm_point_t *pt)
{
    return pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_RTN_UNACK;
}

/* Horn: on while a shown point needs acknowledging (during a flood, INFO points do not count) */
static bool horn_wanted(const hmi_alarm_supervisor_t *sup)
{
    for (uint32_t i = 0; i < sup->points_in_use; i++) {
        const hmi_alarm_point_t *pt = &sup->points[i];
        if (!needs_ack(pt) || (pt->flags & HMI_ALARM_F_HIDDEN)) continue;
        if (sup->flood && pt->peak_severity <= ALARM_SEV_INFO) continue;
        return true;
    }
    return false;
}

uint16_t hmi_alarms_acknowledge_all(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    if (!sup) return 0;
    uint16_t count = 0;
    for (uint32_t i = 0; i < sup->points_in_use; i++) {
        hmi_alarm_point_t *pt = &sup->points[i];
        if (pt->flags & HMI_ALARM_F_HIDDEN) continue;          /* the operator did not see it */
        if (ack_point(pt, now_ms)) count++;
    }
    sup->horn_sounding = horn_wanted(sup) && !sup->horn_silenced;
    if (!sup->horn_sounding) sup->horn_silenced = false;
    return count;
}

bool hmi_alarms_acknowledge_point(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t now_ms)
{
    if (!sup || tag_id >= HMI_MAX_TAGS) return false;
    note_point(sup, tag_id);
    if (!ack_point(&sup->points[tag_id], now_ms)) return false;
    if (!horn_wanted(sup)) { sup->horn_sounding = false; sup->horn_silenced = false; }
    return true;
}

/* ========================================================================= */
/* Shelve / out of service                                                    */
/* ========================================================================= */

bool hmi_alarms_can_hide(hmi_tag_id_t tag_id)
{
    const hmi_tag_meta_t *m = hmi_tag_get_meta(tag_id);
    if (!m) return false;
    return !(m->type == TAG_TYPE_DIGITAL && m->digital_alarm_sev >= ALARM_SEV_ESTOP);
}

bool hmi_alarms_shelve(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t duration_ms, uint32_t now_ms)
{
    if (!sup || tag_id >= HMI_MAX_TAGS || !duration_ms || !hmi_alarms_can_hide(tag_id)) return false;
    note_point(sup, tag_id);
    hmi_alarm_point_t *pt = &sup->points[tag_id];
    pt->flags |= HMI_ALARM_F_SHELVED;
    pt->shelve_until_ms = now_ms + duration_ms;
    char msg[57];
    msgf(msg, sizeof(msg), "Shelved %lu min: %s", (unsigned long)(duration_ms / 60000u),
         point_label(hmi_tag_get_meta(tag_id)));
    hmi_alarms_log_event(sup, now_ms, tag_id, ALARM_SEV_INFO, pt->state, msg);
    return true;
}

bool hmi_alarms_set_out_of_service(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, bool oos, uint32_t now_ms)
{
    if (!sup || tag_id >= HMI_MAX_TAGS || !hmi_alarms_can_hide(tag_id)) return false;
    note_point(sup, tag_id);
    hmi_alarm_point_t *pt = &sup->points[tag_id];
    if (oos == !!(pt->flags & HMI_ALARM_F_OOS)) return false;
    if (oos) pt->flags |= HMI_ALARM_F_OOS; else pt->flags &= (uint8_t)~HMI_ALARM_F_OOS;
    char msg[57];
    msgf(msg, sizeof(msg), "%s: %s", oos ? "Out of service" : "Back in service", point_label(hmi_tag_get_meta(tag_id)));
    hmi_alarms_log_event(sup, now_ms, tag_id, ALARM_SEV_INFO, pt->state, msg);
    return true;
}

static void unshelve(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t now_ms, const char *why)
{
    hmi_alarm_point_t *pt = &sup->points[tag_id];
    pt->flags &= (uint8_t)~HMI_ALARM_F_SHELVED;
    pt->shelve_until_ms = 0;
    if (needs_ack(pt)) sup->horn_silenced = false;              /* it is annunciated again */
    char msg[57];
    msgf(msg, sizeof(msg), "Unshelved (%s): %s", why, point_label(hmi_tag_get_meta(tag_id)));
    hmi_alarms_log_event(sup, now_ms, tag_id, ALARM_SEV_INFO, pt->state, msg);
}

bool hmi_alarms_unshelve(hmi_alarm_supervisor_t *sup, hmi_tag_id_t tag_id, uint32_t now_ms)
{
    if (!sup || tag_id >= HMI_MAX_TAGS) return false;
    hmi_alarm_point_t *pt = &sup->points[tag_id];
    if (pt->flags & HMI_ALARM_F_SHELVED) { unshelve(sup, tag_id, now_ms, "operator"); return true; }
    if (pt->flags & HMI_ALARM_F_OOS) return hmi_alarms_set_out_of_service(sup, tag_id, false, now_ms);
    return false;
}

uint16_t hmi_alarms_hidden(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t *out, uint16_t max)
{
    if (!sup || !out) return 0;
    uint16_t n = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t want = pass == 0 ? HMI_ALARM_F_SHELVED : HMI_ALARM_F_OOS;
        for (uint32_t i = 0; i < sup->points_in_use && n < max; i++) {
            uint8_t f = sup->points[i].flags;
            if (pass == 0 ? (f & want) : ((f & want) && !(f & HMI_ALARM_F_SHELVED))) out[n++] = (hmi_tag_id_t)i;
        }
    }
    return n;
}

/* ========================================================================= */
/* Summary order                                                              */
/* ========================================================================= */

static bool summary_before(const hmi_alarm_point_t *a, const hmi_alarm_point_t *b)
{
    if (a->peak_severity != b->peak_severity) return a->peak_severity > b->peak_severity;
    bool ua = needs_ack(a), ub = needs_ack(b);
    if (ua != ub) return ua;
    return (int32_t)(a->since_ms - b->since_ms) > 0;            /* newer first, wrap-safe */
}

/* Heap sort helpers: the heap keeps the alarm that sorts LAST at its root. */
static bool heap_less(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t a, hmi_tag_id_t b)
{
    if (summary_before(&sup->points[a], &sup->points[b])) return true;
    if (summary_before(&sup->points[b], &sup->points[a])) return false;
    return a < b;                                               /* stable tie-break: lower tag id first */
}

static void heap_sift(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t *h, uint32_t n, uint32_t i)
{
    for (;;) {
        uint32_t l = 2u * i + 1u, r = l + 1u, big = i;
        if (l < n && heap_less(sup, h[big], h[l])) big = l;
        if (r < n && heap_less(sup, h[big], h[r])) big = r;
        if (big == i) return;
        hmi_tag_id_t t = h[i]; h[i] = h[big]; h[big] = t;
        i = big;
    }
}

uint16_t hmi_alarms_sorted(const hmi_alarm_supervisor_t *sup, hmi_tag_id_t *out, uint16_t max)
{
    if (!sup || !out || !max) return 0;
    /* Collect every shown point, sort them all (O(n log n), so a flood of a thousand alarms costs
     * a few thousand comparisons), then keep the first max. */
    static hmi_tag_id_t all[HMI_MAX_TAGS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < sup->points_in_use; i++)
        if (hmi_alarm_point_shown(&sup->points[i])) all[n++] = (hmi_tag_id_t)i;
    for (uint32_t i = n / 2u; i-- > 0;) heap_sift(sup, all, n, i);
    for (uint32_t end = n; end > 1u; end--) {
        hmi_tag_id_t t = all[0]; all[0] = all[end - 1u]; all[end - 1u] = t;
        heap_sift(sup, all, end - 1u, 0);
    }
    uint32_t k = n < max ? n : max;
    for (uint32_t i = 0; i < k; i++) out[i] = all[i];
    return (uint16_t)k;
}

/* ========================================================================= */
/* Guidance                                                                   */
/* ========================================================================= */

static void build_guidance(hmi_emergency_action_t *act, hmi_tag_id_t tag_id, uint8_t sev, uint8_t state, int16_t val)
{
    const hmi_tag_meta_t *meta = hmi_tag_get_meta(tag_id);
    const char *name = point_label(meta);
    act->active = true;
    act->tag_id = tag_id;
    act->severity = sev;
    act->state = state;

    if (meta && meta->type == TAG_TYPE_DIGITAL) {
        msgf(act->title, sizeof(act->title), "%s%s", name,
                 state == ALARM_PT_RTN_UNACK ? " cleared - acknowledge" : " active");
    } else {
        char v[24], lim[24];
        fmt_point(v, sizeof(v), meta, val);
        int16_t limit = meta ? (sev >= ALARM_SEV_CRITICAL ? meta->trip_high : meta->warn_high) : 0;
        fmt_point(lim, sizeof(lim), meta, limit);
        msgf(act->title, sizeof(act->title), "%s %s  %s  (limit %s)", name,
                 sev >= ALARM_SEV_CRITICAL ? "high-high" : "high", v, lim);
    }
    msgf(act->action_step1, sizeof(act->action_step1), "1. %s",
             (meta && meta->action1) ? meta->action1 : "Check the field device and its wiring");
    msgf(act->action_step2, sizeof(act->action_step2), "2. %s",
             (meta && meta->action2) ? meta->action2 : "Acknowledge once the cause is understood");
}

/* ========================================================================= */
/* Evaluation                                                                 */
/* ========================================================================= */

/* A limit is set when it is not 0 and above the bottom of the range */
static bool limit_on(const hmi_tag_meta_t *m, int16_t lim)
{
    return lim != 0 && lim > m->min_val;
}

static uint8_t condition(const hmi_tag_meta_t *meta, const hmi_alarm_point_t *pt, int16_t val)
{
    if (meta->type == TAG_TYPE_DIGITAL)
        return (val != 0) ? meta->digital_alarm_sev : (uint8_t)ALARM_SEV_NONE;
    int32_t deadband = ((int32_t)meta->max_val - (int32_t)meta->min_val) / 100;
    if (deadband < 1) deadband = 1;
    bool trip = limit_on(meta, meta->trip_high), warn = limit_on(meta, meta->warn_high);
    if (trip && val >= meta->trip_high) return ALARM_SEV_CRITICAL;
    if (trip && pt->severity == ALARM_SEV_CRITICAL && (int32_t)val > meta->trip_high - deadband) return ALARM_SEV_CRITICAL;
    if (warn && val >= meta->warn_high) return ALARM_SEV_WARNING;
    if (warn && pt->severity == ALARM_SEV_WARNING && (int32_t)val > meta->warn_high - deadband) return ALARM_SEV_WARNING;
    return ALARM_SEV_NONE;
}

/* ISA-18.2 rate: one more annunciation now; also feeds the point's chatter window */
static void count_annunciation(hmi_alarm_supervisor_t *sup, hmi_tag_id_t id, hmi_alarm_point_t *pt, uint32_t now_ms)
{
    sup->rate_bucket[0]++;
    if ((uint32_t)(now_ms - pt->ann_window_ms) >= HMI_ALARM_CHATTER_MS || pt->ann_count == 0) {
        pt->ann_window_ms = now_ms;
        pt->ann_count = 0;
    }
    if (pt->ann_count < 255) pt->ann_count++;
    if (pt->ann_count >= HMI_ALARM_CHATTER_COUNT && !(pt->flags & HMI_ALARM_F_CHATTER)) {
        pt->flags |= HMI_ALARM_F_CHATTER;
        char msg[57];
        msgf(msg, sizeof(msg), "Chattering: %s", point_label(hmi_tag_get_meta(id)));
        hmi_alarms_log_event(sup, now_ms, id, ALARM_SEV_INFO, pt->state, msg);
    }
}

/* Move the per-minute buckets forward to the current minute (bucket 0 = this minute) */
static void roll_rate(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    uint32_t minute = now_ms / 60000u;
    uint32_t steps = minute - sup->rate_minute;
    if (!steps) return;
    if (steps > 10u) steps = 10u;
    for (int i = 9; i >= 0; i--) sup->rate_bucket[i] = (i >= (int)steps) ? sup->rate_bucket[i - (int)steps] : 0;
    sup->rate_minute = minute;
}

/* Alarms in the last 10 minutes, and the flood state (with hysteresis) */
static void update_rate(hmi_alarm_supervisor_t *sup, uint32_t now_ms)
{
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) sum += sup->rate_bucket[i];
    sup->rate_10min = (uint16_t)(sum > 65535u ? 65535u : sum);
    if (sup->rate_10min > sup->rate_peak_10min) sup->rate_peak_10min = sup->rate_10min;
    char msg[57];
    if (!sup->flood && sup->rate_10min > HMI_ALARM_FLOOD_ON) {
        sup->flood = true;
        sup->flood_count++;
        msgf(msg, sizeof(msg), "Alarm flood: %u alarms in 10 min", (unsigned)sup->rate_10min);
        hmi_alarms_log_event(sup, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_WARNING, ALARM_PT_NORMAL, msg);
    } else if (sup->flood && sup->rate_10min < HMI_ALARM_FLOOD_OFF) {
        sup->flood = false;
        msgf(msg, sizeof(msg), "Alarm flood over: %u alarms in 10 min", (unsigned)sup->rate_10min);
        hmi_alarms_log_event(sup, now_ms, HMI_ALARM_TAG_SYSTEM, ALARM_SEV_INFO, ALARM_PT_NORMAL, msg);
    }
}

/* Apply the state machine to one point with its new condition */
static void transition(hmi_alarm_supervisor_t *sup, hmi_tag_id_t id, const hmi_tag_meta_t *meta,
                       hmi_alarm_point_t *pt, uint8_t cond_sev, int16_t val, uint32_t now_ms)
{
    char msg[57], v[24];
    uint8_t prev = pt->state;
    pt->severity = cond_sev;
    bool shown = !(pt->flags & HMI_ALARM_F_HIDDEN);

    if (prev == ALARM_PT_NORMAL || prev == ALARM_PT_RTN_UNACK) {
        if (cond_sev == ALARM_SEV_NONE) return;
        pt->state = ALARM_PT_UNACK;                              /* new alarm, or it came back */
        pt->peak_severity = cond_sev;
        pt->trip_value = val;
        pt->since_ms = now_ms;
        if (shown) sup->horn_silenced = false;
        count_annunciation(sup, id, pt, now_ms);
        fmt_point(v, sizeof(v), meta, val);
        if (meta->type == TAG_TYPE_DIGITAL) msgf(msg, sizeof(msg), "%s active%s", point_label(meta), prev ? " (re-alarm)" : "");
        else msgf(msg, sizeof(msg), "%s %s %s%s", point_label(meta), cond_sev >= ALARM_SEV_CRITICAL ? "HH" : "H", v,
                  prev ? " (re-alarm)" : "");
        hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
        return;
    }
    /* UNACK or ACKED: the condition is active, or was */
    if (cond_sev != ALARM_SEV_NONE) {
        if (cond_sev <= pt->peak_severity) return;
        pt->state = ALARM_PT_UNACK;                              /* escalation: annunciate again */
        pt->peak_severity = cond_sev;
        pt->trip_value = val;
        if (shown) sup->horn_silenced = false;
        count_annunciation(sup, id, pt, now_ms);
        fmt_point(v, sizeof(v), meta, val);
        msgf(msg, sizeof(msg), "%s HH %s%s", point_label(meta), v, prev == ALARM_PT_ACKED ? " (re-alarm)" : "");
        hmi_alarms_log_event(sup, now_ms, id, cond_sev, pt->state, msg);
        return;
    }
    if (prev == ALARM_PT_UNACK) {
        pt->state = ALARM_PT_RTN_UNACK;                          /* cleared, still to acknowledge; */
        msgf(msg, sizeof(msg), "%s back to normal", point_label(meta));   /* keeps its alarm time */
        hmi_alarms_log_event(sup, now_ms, id, pt->peak_severity, pt->state, msg);
    } else {
        pt->state = ALARM_PT_NORMAL;                             /* acknowledged and cleared */
        pt->peak_severity = ALARM_SEV_NONE;
        pt->since_ms = now_ms;
        msgf(msg, sizeof(msg), "%s cleared", point_label(meta));
        hmi_alarms_log_event(sup, now_ms, id, ALARM_SEV_INFO, pt->state, msg);
    }
}

void hmi_alarms_evaluate(hmi_alarm_supervisor_t *sup, const hmi_tag_table_t *tags, uint32_t now_ms)
{
    if (!sup || !tags) return;
    roll_rate(sup, now_ms);
    if (hmi_tag_ids_in_use() > sup->points_in_use) sup->points_in_use = hmi_tag_ids_in_use();

    for (hmi_tag_id_t id = 0; id < sup->points_in_use; id++) {
        const hmi_tag_meta_t *meta = hmi_tag_get_meta(id);
        if (!meta) continue;
        hmi_alarm_point_t *pt = &sup->points[id];

        /* a shelved point comes back by itself when its time is up */
        if ((pt->flags & HMI_ALARM_F_SHELVED) && (int32_t)(now_ms - pt->shelve_until_ms) >= 0)
            unshelve(sup, id, now_ms, "time up");
        /* chatter flag clears once a whole window passes quietly */
        if ((pt->flags & HMI_ALARM_F_CHATTER) && (uint32_t)(now_ms - pt->ann_window_ms) >= 2u * HMI_ALARM_CHATTER_MS) {
            pt->flags &= (uint8_t)~HMI_ALARM_F_CHATTER;
            pt->ann_count = 0;
        }

        uint8_t q = hmi_tag_read_quality(tags, id);
        if (q != QUALITY_GOOD && q != QUALITY_SIMULATED) continue;   /* bad data: state frozen */
        int16_t val = hmi_tag_read_value(tags, id);
        transition(sup, id, meta, pt, condition(meta, pt, val), val, now_ms);
    }
    update_rate(sup, now_ms);

    /* Summary over the points the operator sees */
    uint16_t active_count = 0, unacked_count = 0, hidden = 0, chatter = 0;
    uint8_t  highest_sev = ALARM_SEV_NONE;
    int      best_guidance_tag = -1;
    uint8_t  best_guidance_sev = ALARM_SEV_NONE;

    for (hmi_tag_id_t id = 0; id < sup->points_in_use; id++) {
        const hmi_alarm_point_t *pt = &sup->points[id];
        if (pt->flags & HMI_ALARM_F_CHATTER) chatter++;
        if (pt->flags & HMI_ALARM_F_HIDDEN) { hidden++; continue; }
        if (pt->state == ALARM_PT_NORMAL) continue;
        if (needs_ack(pt)) unacked_count++;
        if (pt->state == ALARM_PT_UNACK || pt->state == ALARM_PT_ACKED) active_count++;
        if (pt->peak_severity > highest_sev) highest_sev = pt->peak_severity;
        /* guidance: the highest priority, an unacknowledged one first */
        if (pt->peak_severity > best_guidance_sev ||
            (pt->peak_severity == best_guidance_sev && pt->state == ALARM_PT_UNACK)) {
            best_guidance_sev = pt->peak_severity;
            best_guidance_tag = id;
        }
    }

    sup->active_count     = active_count;
    sup->unacked_count    = unacked_count;
    sup->shelved_count    = hidden;
    sup->chatter_count    = chatter;
    sup->highest_severity = (hmi_alarm_sev_t)highest_sev;
    sup->horn_sounding    = !sup->horn_silenced && horn_wanted(sup);

    if (highest_sev == ALARM_SEV_NONE) sup->latch_state = ALARM_LATCH_NORMAL;
    else if (unacked_count > 0)        sup->latch_state = ALARM_LATCH_UNACKNOWLEDGED;
    else                               sup->latch_state = ALARM_LATCH_ACKNOWLEDGED;

    if (best_guidance_tag >= 0 && best_guidance_sev >= ALARM_SEV_WARNING) {
        const hmi_alarm_point_t *bpt = &sup->points[best_guidance_tag];
        build_guidance(&sup->current_action, (hmi_tag_id_t)best_guidance_tag,
                       best_guidance_sev, bpt->state, bpt->trip_value);
    } else {
        sup->current_action.active = false;
    }
}

