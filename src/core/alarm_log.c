/**
 * @file alarm_log.c
 * @brief Append-only, hash-chained alarm audit log (Win32 file API, no heap)
 * @standards ISA-18.2 audit trail / IEC 62443-4-2 CR 2.8
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../include/core/alarm_log.h"
#include "../../include/core/crc32.h"

static HANDLE                s_file = INVALID_HANDLE_VALUE;
static char                  s_path[MAX_PATH];
static uint32_t              s_max_bytes = HMI_ALARM_LOG_MAX_BYTES;
static uint32_t              s_last_written_seq;
static uint32_t              s_chain = HMI_ALARM_LOG_GENESIS;
static hmi_alarm_log_stats_t s_stats;

#define HEADER_PREFIX "# industrial_hmi alarm log v2;chain="
#define HEADER_SUFFIX ";seq,utc,uptime_ms,tag,severity,state,message,crc32c\r\n"

/* ------------------------------------------------------------------------- */
/* Pure helpers                                                               */
/* ------------------------------------------------------------------------- */

static uint32_t chained_crc(uint32_t prev, const char *body, size_t len)
{
    char buf[320];
    if (len > sizeof(buf) - 9) len = sizeof(buf) - 9;
    snprintf(buf, sizeof(buf), "%08lX", (unsigned long)prev);
    memcpy(buf + 8, body, len);
    return hmi_crc32c((const uint8_t *)buf, len + 8);
}

static bool parse_hex8(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

size_t hmi_alarm_log_format_line(char *out, size_t cap, const hmi_alarm_record_t *rec,
                                 const char *utc_iso, uint32_t prev_crc, uint32_t *out_crc)
{
    if (!out || !rec || !utc_iso || cap < 64) return 0;

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

    uint32_t crc = chained_crc(prev_crc, body, (size_t)n);
    int total = snprintf(out, cap, "%s,%08lX\r\n", body, (unsigned long)crc);
    if (total <= 0 || (size_t)total >= cap) return 0;
    if (out_crc) *out_crc = crc;
    return (size_t)total;
}

bool hmi_alarm_log_verify_line(const char *line, size_t len, uint32_t *chain)
{
    if (!line || !chain) return false;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (len == 0) return false;

    if (line[0] == '#') {
        size_t hp = sizeof(HEADER_PREFIX) - 1;
        uint32_t c;
        if (len < hp + 8 || strncmp(line, HEADER_PREFIX, hp) != 0 || !parse_hex8(line + hp, &c)) return false;
        *chain = c;
        return true;
    }

    if (len < 10 || line[len - 9] != ',') return false;
    uint32_t stored;
    if (!parse_hex8(line + len - 8, &stored)) return false;
    if (chained_crc(*chain, line, len - 9) != stored) return false;
    *chain = stored;
    return true;
}

bool hmi_alarm_log_verify_file(const char *path, uint32_t *out_lines, uint32_t *out_bad, uint32_t *out_first_bad)
{
    uint32_t lines = 0, bad = 0, first_bad = 0;
    if (out_lines) *out_lines = 0;
    if (out_bad) *out_bad = 0;
    if (out_first_bad) *out_first_bad = 0;
    if (!path) return false;

    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    static char buf[4096];
    static char line[512];
    size_t ll = 0;
    bool overflow = false;
    uint32_t chain = HMI_ALARM_LOG_GENESIS;
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(f, buf, sizeof(buf), &got, NULL) || got == 0) break;
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\n') {
                lines++;
                if (overflow || !hmi_alarm_log_verify_line(line, ll, &chain) ||
                    (lines > 1 && line[0] == '#' && ll > 0)) {
                    /* a header is only legal as the first line of a file */
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
    CloseHandle(f);

    if (out_lines) *out_lines = lines;
    if (out_bad) *out_bad = bad;
    if (out_first_bad) *out_first_bad = first_bad;
    return lines > 0 && bad == 0;
}

/* ------------------------------------------------------------------------- */
/* File I/O                                                                   */
/* ------------------------------------------------------------------------- */

static void utc_now(char *out, size_t cap)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    snprintf(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

static bool write_raw(const char *buf, size_t len)
{
    if (s_file == INVALID_HANDLE_VALUE || !buf || len == 0) return false;
    DWORD written = 0;
    if (!WriteFile(s_file, buf, (DWORD)len, &written, NULL) || written != len) {
        s_stats.write_errors++;
        return false;
    }
    s_stats.file_bytes += (uint32_t)len;
    return true;
}

static void write_header(void)
{
    char h[160];
    int n = snprintf(h, sizeof(h), "%s%08lX%s", HEADER_PREFIX, (unsigned long)s_chain, HEADER_SUFFIX);
    if (n > 0) write_raw(h, (size_t)n);
}

/* Recover the chain value from the last line of an existing file. */
static bool recover_tail(HANDLE f, uint32_t size)
{
    char tail[400];
    DWORD want = size < sizeof(tail) ? size : (DWORD)sizeof(tail);
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)size - want;
    DWORD got = 0;
    if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN) || !ReadFile(f, tail, want, &got, NULL) || got != want) return false;

    size_t end = got;
    while (end > 0 && (tail[end - 1] == '\r' || tail[end - 1] == '\n')) end--;
    if (end == 0 || end == got) return false;          /* file must end with a newline */
    size_t start = end;
    while (start > 0 && tail[start - 1] != '\n') start--;
    if (start == 0 && want < size) return false;        /* line longer than the tail window */

    uint32_t c = 0;
    const char *ln = tail + start;
    size_t len = end - start;
    if (ln[0] == '#') {
        size_t hp = sizeof(HEADER_PREFIX) - 1;
        if (len < hp + 8 || strncmp(ln, HEADER_PREFIX, hp) != 0 || !parse_hex8(ln + hp, &c)) return false;
    } else {
        if (len < 10 || ln[len - 9] != ',' || !parse_hex8(ln + len - 8, &c)) return false;
    }
    s_chain = c;
    return true;
}

static bool open_file(bool fresh)
{
    s_file = CreateFileA(s_path, FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ, NULL,
                         fresh ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    s_stats.file_bytes = GetFileSizeEx(s_file, &sz) ? (uint32_t)sz.QuadPart : 0;
    if (s_stats.file_bytes == 0) {
        write_header();
    } else if (recover_tail(s_file, s_stats.file_bytes)) {
        s_stats.chain_resumed = true;
    } else {
        /* Unreadable tail (e.g. torn last write): rotate it away and start a fresh file
         * so the damaged log stays available for inspection. */
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
        char backup[MAX_PATH + 16];
        snprintf(backup, sizeof(backup), "%s.damaged", s_path);
        MoveFileExA(s_path, backup, MOVEFILE_REPLACE_EXISTING);
        s_chain = HMI_ALARM_LOG_GENESIS;
        return open_file(true);
    }
    s_stats.open = true;
    return true;
}

static void rotate_if_needed(size_t next_len)
{
    if (s_file == INVALID_HANDLE_VALUE || s_stats.file_bytes + next_len <= s_max_bytes) return;
    CloseHandle(s_file);
    s_file = INVALID_HANDLE_VALUE;
    char backup[MAX_PATH + 8];
    snprintf(backup, sizeof(backup), "%s.1", s_path);
    MoveFileExA(s_path, backup, MOVEFILE_REPLACE_EXISTING);
    if (open_file(true)) s_stats.rotations++;
    else s_stats.open = false;
}

bool hmi_alarm_log_open(const char *path, uint32_t max_bytes)
{
    hmi_alarm_log_close();
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_written_seq = 0;
    s_chain = HMI_ALARM_LOG_GENESIS;
    if (!path || !path[0] || strlen(path) >= sizeof(s_path)) return false;
    memcpy(s_path, path, strlen(path) + 1);
    s_max_bytes = max_bytes > 4096u ? max_bytes : 4096u;
    if (!open_file(false)) { s_stats.open = false; return false; }
    s_stats.chain_tail = s_chain;
    return true;
}

static void append_record(const hmi_alarm_record_t *rec)
{
    char utc[48], line[320];
    uint32_t crc = 0;
    utc_now(utc, sizeof(utc));
    size_t len = hmi_alarm_log_format_line(line, sizeof(line), rec, utc, s_chain, &crc);
    if (!len) { s_stats.write_errors++; return; }
    rotate_if_needed(len);
    /* rotation may have started a new file: re-chain the line against its header */
    len = hmi_alarm_log_format_line(line, sizeof(line), rec, utc, s_chain, &crc);
    if (len && write_raw(line, len)) {
        s_chain = crc;
        s_stats.chain_tail = crc;
        s_stats.records_written++;
    }
}

void hmi_alarm_log_sync(const hmi_alarm_supervisor_t *snap)
{
    if (!snap || s_file == INVALID_HANDLE_VALUE) return;
    if (snap->journal_total == s_last_written_seq) return;
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
}

void hmi_alarm_log_close(void)
{
    if (s_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(s_file);
        CloseHandle(s_file);
        s_file = INVALID_HANDLE_VALUE;
    }
    s_stats.open = false;
}

void hmi_alarm_log_get_stats(hmi_alarm_log_stats_t *out)
{
    if (out) *out = s_stats;
}
