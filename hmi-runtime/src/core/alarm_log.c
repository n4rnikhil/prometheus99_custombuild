/**
 * @file alarm_log.c
 * @brief Append-only, keyed hash-chained alarm and audit log (portable file layer, no heap)
 * @standards ISA-18.2 audit trail / IEC 62443-4-2 CR 2.8, CR 3.9
 *
 * Format and guarantees: see alarm_log.h.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "core/alarm_log.h"
#include "core/crc32.h"
#include "core/sha256.h"
#include "core/device_key.h"
#include "hal/fs.h"
#include "hal/sys.h"
#include "hal/timer.h"

#define MAC_HEX   HMI_ALARM_LOG_MAC_HEX
#define TAIL_LEN  (1u + MAC_HEX)                /* ",<mac>" */

static hal_file_t           *s_file;
static char                  s_path[HMI_PATH_MAX];
static uint32_t              s_max_bytes = HMI_ALARM_LOG_MAX_BYTES;
static uint32_t              s_last_written_seq;
static char                  s_chain[MAC_HEX + 1];
static hmi_alarm_log_stats_t s_stats;
static uint8_t               s_key[HMI_DEVICE_KEY_LEN];
static bool                  s_have_key;
static char                  s_key_id[9] = "NONE";
static char                  s_note[160];
static bool                  s_sync_pending;       /* written to the OS, not yet forced to the medium */
static bool                  s_sync_urgent;        /* a stop / critical record is waiting */
static uint32_t              s_last_sync_ms;

/* Longest record body (before ",<mac>") that hmi_alarm_log_format_line() can produce: it formats
 * into a 256-byte buffer. Keep in sync with `char body[256]` below. */
#define HMI_ALARM_LOG_MAX_BODY 255u

#define V3_PREFIX      "# industrial_hmi alarm log v3;key="
#define V3_PREV        ";prev="
#define V3_COLUMNS     ";columns=seq|utc|uptime_ms|tag|severity|state|message|mac"
#define V2_PREFIX      "# industrial_hmi alarm log v2;chain="
#define ZERO_MAC       "00000000000000000000000000000000"

/* ------------------------------------------------------------------------- */
/* Key and MAC                                                                */
/* ------------------------------------------------------------------------- */

void hmi_alarm_log_set_key(const uint8_t *key32)
{
    if (key32) {
        memcpy(s_key, key32, HMI_DEVICE_KEY_LEN);
        s_have_key = true;
        hmi_device_key_id(s_key, s_key_id);
    } else {
        memset(s_key, 0, sizeof(s_key));
        s_have_key = false;
        snprintf(s_key_id, sizeof(s_key_id), "NONE");
    }
}

static void line_mac(const char *prev_mac, const char *body, size_t len, char out[MAC_HEX + 1])
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t m[32];
    hmi_hmac_sha256_t c;
    hmi_hmac_sha256_init(&c, s_key, sizeof(s_key));
    hmi_hmac_sha256_update(&c, prev_mac, MAC_HEX);
    hmi_hmac_sha256_update(&c, body, len);
    hmi_hmac_sha256_final(&c, m);
    for (unsigned i = 0; i < MAC_HEX / 2u; i++) {
        out[2 * i] = hex[m[i] >> 4];
        out[2 * i + 1] = hex[m[i] & 15u];
    }
    out[MAC_HEX] = '\0';
}

static bool is_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static uint32_t hex_u32(const char *s)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'A' + 10);
    }
    return v;
}

/* ------------------------------------------------------------------------- */
/* Pure helpers                                                               */
/* ------------------------------------------------------------------------- */

size_t hmi_alarm_log_format_line(char *out, size_t cap, const hmi_alarm_record_t *rec,
                                 const char *utc_iso, const char *prev_mac, char out_mac[33])
{
    if (!out || !rec || !utc_iso || !prev_mac || cap < 64) return 0;

    char tag[8];
    if (rec->tag_id == HMI_ALARM_TAG_SYSTEM) snprintf(tag, sizeof(tag), "SYS");
    else snprintf(tag, sizeof(tag), "%u", (unsigned)rec->tag_id);

    char msg[sizeof(rec->message)];
    size_t i = 0;
    for (; i + 1 < sizeof(msg) && rec->message[i]; i++) {
        char c = rec->message[i];
        msg[i] = (c == '"' || c == ',' || (unsigned char)c < 32 || (unsigned char)c > 126) ? ' ' : c;
    }
    msg[i] = '\0';

    char body[256];
    int n = snprintf(body, sizeof(body), "%lu,%.47s,%lu,%s,%s,%s,\"%s\"",
                     (unsigned long)rec->seq, utc_iso, (unsigned long)rec->timestamp_ms, tag,
                     hmi_alarm_sev_name(rec->severity), hmi_alarm_state_name(rec->state), msg);
    if (n <= 0 || (size_t)n >= sizeof(body)) return 0;

    char mac[MAC_HEX + 1];
    line_mac(prev_mac, body, (size_t)n, mac);
    int total = snprintf(out, cap, "%s,%s\r\n", body, mac);
    if (total <= 0 || (size_t)total >= cap) return 0;
    if (out_mac) memcpy(out_mac, mac, sizeof(mac));
    return (size_t)total;
}

static size_t format_header(char *out, size_t cap, const char *prev_mac, char out_mac[MAC_HEX + 1])
{
    char body[200];
    int n = snprintf(body, sizeof(body), "%s%s%s%.32s%s", V3_PREFIX, s_key_id, V3_PREV, prev_mac, V3_COLUMNS);
    if (n <= 0 || (size_t)n >= sizeof(body)) return 0;
    line_mac(prev_mac, body, (size_t)n, out_mac);
    int total = snprintf(out, cap, "%s,%s\r\n", body, out_mac);
    return (total <= 0 || (size_t)total >= cap) ? 0 : (size_t)total;
}

/* --- v2 (CRC32-C chain, unkeyed): verification only --- */
static uint32_t v2_chained_crc(uint32_t prev, const char *body, size_t len)
{
    char buf[320];
    if (len > sizeof(buf) - 9) len = sizeof(buf) - 9;
    snprintf(buf, sizeof(buf), "%08lX", (unsigned long)prev);
    memcpy(buf + 8, body, len);
    return hmi_crc32c((const uint8_t *)buf, len + 8);
}

static bool v2_verify_line(const char *line, size_t len, uint32_t *chain)
{
    if (line[0] == '#') {
        size_t hp = sizeof(V2_PREFIX) - 1;
        if (len < hp + 8 || strncmp(line, V2_PREFIX, hp) != 0 || !is_hex(line + hp, 8)) return false;
        *chain = hex_u32(line + hp);
        return true;
    }
    if (len < 10 || line[len - 9] != ',' || len - 9 > HMI_ALARM_LOG_MAX_BODY || !is_hex(line + len - 8, 8)) return false;
    uint32_t stored = hex_u32(line + len - 8);
    if (v2_chained_crc(*chain, line, len - 9) != stored) return false;
    *chain = stored;
    return true;
}

/* --- v3 --- */
typedef struct {
    int  format;                  /* 0 = unknown yet, 2, 3 */
    bool key_mismatch;
    char file_key[9];
    char mac[MAC_HEX + 1];
    uint32_t v2_chain;
} verify_state_t;

static bool v3_verify_line(const char *line, size_t len, verify_state_t *vs, bool first_line)
{
    if (len < TAIL_LEN + 1 || line[len - TAIL_LEN] != ',' || !is_hex(line + len - MAC_HEX, MAC_HEX)) return false;
    size_t body_len = len - TAIL_LEN;
    const char *prev = vs->mac;

    if (line[0] == '#') {
        if (!first_line) return false;                     /* a header is only legal on line 1 */
        size_t kp = sizeof(V3_PREFIX) - 1, pp = sizeof(V3_PREV) - 1;
        if (body_len < kp + 8 + pp + MAC_HEX || strncmp(line, V3_PREFIX, kp) != 0) return false;
        const char *k = line + kp;
        size_t klen = strcspn(k, ";");
        if (klen == 0 || klen > 8) return false;
        memcpy(vs->file_key, k, klen);
        vs->file_key[klen] = '\0';
        const char *p = k + klen;
        if (strncmp(p, V3_PREV, pp) != 0 || !is_hex(p + pp, MAC_HEX)) return false;
        memcpy(vs->mac, p + pp, MAC_HEX);                  /* the header is signed against its own prev */
        vs->mac[MAC_HEX] = '\0';
        if (strcmp(vs->file_key, s_key_id) != 0) { vs->key_mismatch = true; return false; }
    } else if (first_line || body_len > HMI_ALARM_LOG_MAX_BODY) {
        return false;                                      /* v3 files always start with a header */
    }

    char mac[MAC_HEX + 1];
    line_mac(prev, line, body_len, mac);
    if (!hmi_ct_equal(mac, line + len - MAC_HEX, MAC_HEX)) return false;
    memcpy(vs->mac, mac, sizeof(mac));
    return true;
}

static bool verify_line(const char *line, size_t len, verify_state_t *vs, bool first_line)
{
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (len == 0) return false;
    if (vs->format == 0) {
        if (strncmp(line, V2_PREFIX, sizeof(V2_PREFIX) - 1) == 0) { vs->format = 2; vs->v2_chain = HMI_ALARM_LOG_GENESIS; }
        else vs->format = 3;
    }
    if (vs->format == 2) {
        if (!first_line && line[0] == '#') return false;
        return v2_verify_line(line, len, &vs->v2_chain);
    }
    return v3_verify_line(line, len, vs, first_line);
}

bool hmi_alarm_log_verify_file(const char *path, uint32_t *out_lines, uint32_t *out_bad, uint32_t *out_first_bad)
{
    uint32_t lines = 0, bad = 0, first_bad = 0;
    if (out_lines) *out_lines = 0;
    if (out_bad) *out_bad = 0;
    if (out_first_bad) *out_first_bad = 0;
    snprintf(s_note, sizeof(s_note), "cannot open the file");
    if (!path) return false;

    hal_file_t *f = hal_file_open(path, HAL_FILE_READ);
    if (!f) return false;

    static char buf[4096];
    static char line[512];
    static verify_state_t vs;
    memset(&vs, 0, sizeof(vs));
    memcpy(vs.mac, ZERO_MAC, sizeof(vs.mac));
    size_t ll = 0;
    bool overflow = false;
    for (;;) {
        int got = hal_file_read(f, buf, sizeof(buf));
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n') {
                lines++;
                if (overflow || !verify_line(line, ll, &vs, lines == 1)) {
                    bad++;
                    if (!first_bad) first_bad = lines;
                }
                ll = 0;
                overflow = false;
            } else if (ll < sizeof(line)) {
                line[ll++] = c;
            } else {
                overflow = true;
            }
        }
    }
    if (ll > 0) {    /* last line without newline: truncated write */
        lines++;
        bad++;
        if (!first_bad) first_bad = lines;
    }
    hal_file_close(f);

    if (out_lines) *out_lines = lines;
    if (out_bad) *out_bad = bad;
    if (out_first_bad) *out_first_bad = first_bad;
    bool ok = lines > 0 && bad == 0;

    if (vs.format == 2)
        snprintf(s_note, sizeof(s_note), "format v2: CRC chain only - shows accidental damage, NOT deliberate edits");
    else if (vs.key_mismatch)
        snprintf(s_note, sizeof(s_note), "signed with key %s, this installation has key %s", vs.file_key, s_key_id);
    else if (!s_have_key || strcmp(vs.file_key, "NONE") == 0)
        snprintf(s_note, sizeof(s_note), "format v3 without a device key: integrity only, NOT authenticated");
    else if (ok)
        snprintf(s_note, sizeof(s_note), "format v3, HMAC-SHA256 chain, key %s", s_key_id);
    else
        snprintf(s_note, sizeof(s_note), "format v3, key %s: chain broken (edited, inserted, removed or reordered lines)", s_key_id);
    return ok && !(vs.format == 3 && vs.key_mismatch);
}

const char *hmi_alarm_log_verify_note(void) { return s_note; }

/* ------------------------------------------------------------------------- */
/* File I/O                                                                   */
/* ------------------------------------------------------------------------- */

static bool write_raw(const char *buf, size_t len)
{
    if (!s_file || !buf || len == 0) return false;
    if (!hal_file_write(s_file, buf, len)) {
        s_stats.write_errors++;
        return false;
    }
    s_stats.file_bytes += (uint32_t)len;
    return true;
}

static void write_header(void)
{
    char h[256], mac[MAC_HEX + 1];
    size_t n = format_header(h, sizeof(h), s_chain, mac);
    if (n && write_raw(h, n)) memcpy(s_chain, mac, sizeof(mac));
}

/* Recover the chain value from the last line of an existing v3 file. */
static bool recover_tail(hal_file_t *f, uint32_t size)
{
    char tail[400];
    size_t want = size < sizeof(tail) ? size : sizeof(tail);
    if (!hal_file_pread(f, size - (uint32_t)want, tail, want)) return false;

    size_t end = want;
    while (end > 0 && (tail[end - 1] == '\r' || tail[end - 1] == '\n')) end--;
    if (end == 0 || end == want) return false;          /* file must end with a newline */
    size_t start = end;
    while (start > 0 && tail[start - 1] != '\n') start--;
    if (start == 0 && want < size) return false;        /* line longer than the tail window */

    const char *ln = tail + start;
    size_t len = end - start;
    if (len < TAIL_LEN + 1 || ln[len - TAIL_LEN] != ',' || !is_hex(ln + len - MAC_HEX, MAC_HEX)) return false;
    memcpy(s_chain, ln + len - MAC_HEX, MAC_HEX);
    s_chain[MAC_HEX] = '\0';
    return true;
}

/* An existing file in another format or signed with another key is moved aside, never appended to. */
static bool file_is_current_format(hal_file_t *f, uint32_t size, bool *is_v2)
{
    char head[sizeof(V3_PREFIX) + 8];
    *is_v2 = false;
    size_t n = sizeof(head) - 1;
    if (size < n || !hal_file_pread(f, 0, head, n)) return false;
    head[n] = '\0';
    if (strncmp(head, V2_PREFIX, sizeof(V2_PREFIX) - 1) == 0) { *is_v2 = true; return false; }
    size_t kp = sizeof(V3_PREFIX) - 1;
    return strncmp(head, V3_PREFIX, kp) == 0 && strncmp(head + kp, s_key_id, strlen(s_key_id)) == 0;
}

static void move_aside(const char *suffix)
{
    char backup[HMI_PATH_MAX + 16];
    snprintf(backup, sizeof(backup), "%s%s", s_path, suffix);
    hal_file_replace(s_path, backup);
}

static bool open_file(bool fresh)
{
    if (fresh) {                                  /* start empty: drop whatever is at the path */
        hal_file_t *t = hal_file_open(s_path, HAL_FILE_WRITE_NEW);
        hal_file_close(t);
    }
    s_file = hal_file_open(s_path, HAL_FILE_APPEND);
    if (!s_file) return false;
    s_stats.file_bytes = hal_file_size(s_file);
    if (s_stats.file_bytes == 0) {
        write_header();
    } else {
        bool is_v2 = false;
        bool current = file_is_current_format(s_file, s_stats.file_bytes, &is_v2);
        if (current && recover_tail(s_file, s_stats.file_bytes)) {
            s_stats.chain_resumed = true;
        } else {
            /* v2 file, another key, or an unreadable tail (e.g. torn last write): keep the old
             * file for inspection and start a fresh one. */
            hal_file_close(s_file);
            s_file = NULL;
            move_aside(is_v2 ? ".v2" : ".damaged");
            if (is_v2) s_stats.legacy_rotated = true;
            memcpy(s_chain, ZERO_MAC, sizeof(s_chain));
            return open_file(true);
        }
    }
    s_stats.open = true;
    return true;
}

static void rotate_if_needed(size_t next_len)
{
    if (!s_file || s_stats.file_bytes + next_len <= s_max_bytes) return;
    hal_file_close(s_file);
    s_file = NULL;
    move_aside(".1");
    if (open_file(true)) s_stats.rotations++;   /* the new header carries s_chain as prev */
    else s_stats.open = false;
}

bool hmi_alarm_log_open(const char *path, uint32_t max_bytes)
{
    hmi_alarm_log_close();
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_written_seq = 0;
    memcpy(s_chain, ZERO_MAC, sizeof(s_chain));
    s_stats.authenticated = s_have_key;
    if (!path || !path[0] || strlen(path) >= sizeof(s_path)) return false;
    memcpy(s_path, path, strlen(path) + 1);
    s_max_bytes = max_bytes > 4096u ? max_bytes : 4096u;
    if (!open_file(false)) { s_stats.open = false; return false; }
    s_stats.chain_tail = hex_u32(s_chain);
    return true;
}

/* Wall-clock time of the EVENT, not of the write: the journal stamps records with the monotonic
 * uptime clock, and the log may write them a few frames (or, during a UI stall, seconds) later. */
static void event_utc(const hmi_alarm_record_t *rec, char *out, size_t cap)
{
    uint64_t now_utc = hal_utc_ms();
    uint32_t age = hmi_timer_get_ms() - rec->timestamp_ms;          /* wrap-safe */
    if (age > 86400000u || now_utc < age) age = 0;                  /* nonsense: stamp with now */
    hal_utc_format(now_utc - age, out, cap);
}

static void append_record(const hmi_alarm_record_t *rec)
{
    char utc[48], line[320], mac[MAC_HEX + 1];
    event_utc(rec, utc, sizeof(utc));
    size_t len = hmi_alarm_log_format_line(line, sizeof(line), rec, utc, s_chain, mac);
    if (!len) { s_stats.write_errors++; return; }
    rotate_if_needed(len);
    /* rotation may have started a new file: re-chain the line against its header */
    len = hmi_alarm_log_format_line(line, sizeof(line), rec, utc, s_chain, mac);
    if (len && write_raw(line, len)) {
        memcpy(s_chain, mac, sizeof(mac));
        s_stats.chain_tail = hex_u32(mac);
        s_stats.records_written++;
        s_sync_pending = true;
        if (rec->severity >= ALARM_SEV_CRITICAL) s_sync_urgent = true;
    }
}

/* Bounded durability: see HMI_ALARM_LOG_SYNC_MS. */
static void sync_policy(void)
{
    if (!s_file || !s_sync_pending) return;
    uint32_t now = hmi_timer_get_ms();
    if (!s_sync_urgent && (uint32_t)(now - s_last_sync_ms) < HMI_ALARM_LOG_SYNC_MS) return;
    if (!hal_file_sync(s_file)) s_stats.write_errors++;
    else s_stats.medium_syncs++;
    s_last_sync_ms = now;
    s_sync_pending = false;
    s_sync_urgent = false;
}

void hmi_alarm_log_sync(const hmi_alarm_supervisor_t *snap)
{
    if (!snap || !s_file) return;
    if (snap->journal_total == s_last_written_seq) { sync_policy(); return; }
    if (snap->journal_total < s_last_written_seq) s_last_written_seq = 0;   /* supervisor restarted */

    uint32_t oldest = (snap->journal_total > snap->journal_count) ? snap->journal_total - snap->journal_count + 1u : 1u;
    if (s_last_written_seq + 1u < oldest) {
        hmi_alarm_record_t ovr;
        memset(&ovr, 0, sizeof(ovr));
        uint32_t lost = oldest - 1u - s_last_written_seq;
        ovr.seq = s_last_written_seq + 1u;
        ovr.tag_id = HMI_ALARM_TAG_SYSTEM;
        ovr.severity = ALARM_SEV_WARNING;
        ovr.state = ALARM_PT_NORMAL;
        snprintf(ovr.message, sizeof(ovr.message), "LOG OVERRUN: %lu RECORDS LOST", (unsigned long)lost);
        append_record(&ovr);
        s_stats.records_lost += lost;
        s_last_written_seq = oldest - 1u;
    }
    for (uint32_t seq = s_last_written_seq + 1u; seq <= snap->journal_total; seq++) {
        const hmi_alarm_record_t *rec = hmi_alarms_journal_by_seq(snap, seq);
        if (rec) append_record(rec);
        s_last_written_seq = seq;
    }
    if (!hal_file_flush(s_file)) s_stats.write_errors++;   /* to the OS now: survives a runtime crash */
    sync_policy();                                          /* to the medium: bounded rate */
}

void hmi_alarm_log_close(void)
{
    if (s_file) {
        hal_file_sync(s_file);
        hal_file_close(s_file);
        s_file = NULL;
    }
    s_sync_pending = false;
    s_sync_urgent = false;
    s_stats.open = false;
}

void hmi_alarm_log_get_stats(hmi_alarm_log_stats_t *out)
{
    if (out) *out = s_stats;
}
