/**
 * @file alarm_log.h
 * @brief Append-only, keyed hash-chained alarm and audit log on disk (no heap)
 * @standards ISA-18.2 audit trail / IEC 62443-4-2 CR 2.8 (auditable events), CR 3.9 (protection
 *            of audit information)
 *
 * File format v3: ASCII CSV, CRLF line endings. Every line ends with ",<mac>", where mac is
 * 32 hex digits: the first 128 bits of
 *
 *     HMAC-SHA256(device key, <previous mac as 32 hex digits> || <the line before the final ','>)
 *
 *   Header (first line of every file):
 *     # industrial_hmi alarm log v3;key=<key id>;prev=<32 hex>;columns=seq|utc|...|mac,<mac>
 *     key  = hmi_device_key_id() of the key that signed the file ("NONE" = no key was available)
 *     prev = the mac of the last line before this file: 32 zeros for a brand-new log, or the
 *            tail of the previous file after rotation. The header is signed with prev as its
 *            previous mac, so the first lines of a file cannot be cut off unnoticed.
 *   Record:
 *     <seq>,<utc>,<uptime_ms>,<tag>,<SEV>,<STATE>,"<message>",<mac>
 *
 * Editing, deleting, inserting or reordering any line breaks the chain from that point on.
 * Without the device key nobody can recompute it. What a chain cannot show: lines cut off the
 * END of the newest file, or a whole rotated file deleted. Copy the log off the panel (or to
 * syslog / a historian) if those matter.
 *
 * Files written by format v2 (CRC32-C chain, no key) are still verified, and reported as
 * integrity-checked only. An existing v2 file is renamed to "<path>.v2" when the log opens.
 *
 * On open, the chain continues from the last line already in the file, so the log stays
 * verifiable across restarts. Rotation (beyond max_bytes) renames the file to "<path>.1" and
 * starts a new file whose header carries the chain value.
 *
 * Threading: call everything from ONE thread (the UI thread).
 */

#ifndef INDUSTRIAL_HMI_ALARM_LOG_H
#define INDUSTRIAL_HMI_ALARM_LOG_H

#include "config.h"
#include "core/alarms.h"

#define HMI_ALARM_LOG_GENESIS    0xC001CAFEu   /* v2 only */
#define HMI_ALARM_LOG_MAC_HEX    32u           /* 128-bit truncated HMAC-SHA256 */

typedef struct {
    bool     open;
    bool     chain_resumed;     /* continued an existing log on open */
    bool     authenticated;     /* lines are signed with a device key */
    bool     legacy_rotated;    /* a v2 file was moved to <path>.v2 on open */
    uint32_t records_written;   /* this run */
    uint32_t records_lost;      /* in-memory overruns this run */
    uint32_t write_errors;
    uint32_t rotations;
    uint32_t file_bytes;
    uint32_t chain_tail;        /* first 32 bits of the last mac written (display only) */
    uint32_t medium_syncs;      /* forced writes to the medium (bounded by HMI_ALARM_LOG_SYNC_MS) */
} hmi_alarm_log_stats_t;

/* Key for signing and verifying. Call before open / verify. NULL = no key (lines are still
 * chained, with an all-zero key, and the header says key=NONE). The key is copied. */
void hmi_alarm_log_set_key(const uint8_t *key32);

bool hmi_alarm_log_open(const char *path, uint32_t max_bytes);
void hmi_alarm_log_sync(const hmi_alarm_supervisor_t *snap);
void hmi_alarm_log_close(void);
void hmi_alarm_log_get_stats(hmi_alarm_log_stats_t *out);

/* --- Pure helpers (tests, --verify-log) --- */

/* Format one record line (with CRLF) chained to prev_mac (32 hex digits, no terminator needed).
 * Returns the length, or 0 if cap is too small. out_mac receives this line's mac (33 bytes
 * with the terminator). */
size_t hmi_alarm_log_format_line(char *out, size_t cap, const hmi_alarm_record_t *rec,
                                 const char *utc_iso, const char *prev_mac, char out_mac[33]);

/* Verify a whole file with the key set by hmi_alarm_log_set_key(). Returns true if every
 * line verifies. Counts are optional. *out_first_bad receives the 1-based line number of
 * the first failure (0 if none). */
bool hmi_alarm_log_verify_file(const char *path, uint32_t *out_lines, uint32_t *out_bad,
                               uint32_t *out_first_bad);

/* One line explaining the last hmi_alarm_log_verify_file() result (key, format, why it failed). */
const char *hmi_alarm_log_verify_note(void);

#endif /* INDUSTRIAL_HMI_ALARM_LOG_H */
