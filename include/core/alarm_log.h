/**
 * @file alarm_log.h
 * @brief Append-only, hash-chained alarm audit log on disk (no heap)
 * @standards ISA-18.2 audit trail / IEC 62443-4-2 CR 2.8 (auditable events)
 *
 * File format: ASCII CSV, CRLF line endings.
 *   Header (first line of every file):
 *     # industrial_hmi alarm log v2;chain=XXXXXXXX;seq,utc,uptime_ms,tag,severity,state,message,crc32c
 *     chain = CRC of the last record before this file (0xC001CAFE for a brand-new log,
 *             or the tail of the previous file after rotation)
 *   Record:
 *     <seq>,<utc>,<uptime_ms>,<tag>,<SEV>,<STATE>,"<message>",<crc>
 *     crc = CRC32-C over the 8 hex digits of the PREVIOUS record's crc followed by
 *           every byte of this line before the final ','
 *
 * Because every CRC depends on the one before it, editing, deleting, inserting or
 * reordering any record breaks the chain from that point on, and verification
 * reports it. CRC32-C is an integrity check, not a cryptographic signature: it
 * detects accidental damage and casual edits, not an attacker who recomputes the
 * chain. (Use an HMAC with a device key for that.)
 *
 * On open, the chain continues from the last record already in the file, so the
 * log stays verifiable across restarts. Rotation (beyond max_bytes) renames the
 * file to "<path>.1" and starts a new file whose header carries the chain value.
 *
 * Threading: call everything from ONE thread (the UI thread).
 */

#ifndef INDUSTRIAL_HMI_ALARM_LOG_H
#define INDUSTRIAL_HMI_ALARM_LOG_H

#include "../config.h"
#include "alarms.h"

#define HMI_ALARM_LOG_GENESIS 0xC001CAFEu

typedef struct {
    bool     open;
    bool     chain_resumed;     /* continued an existing log on open */
    uint32_t records_written;   /* this run */
    uint32_t records_lost;      /* in-memory overruns this run */
    uint32_t write_errors;
    uint32_t rotations;
    uint32_t file_bytes;
    uint32_t chain_tail;        /* CRC of the last record written */
} hmi_alarm_log_stats_t;

bool hmi_alarm_log_open(const char *path, uint32_t max_bytes);
void hmi_alarm_log_sync(const hmi_alarm_supervisor_t *snap);
void hmi_alarm_log_close(void);
void hmi_alarm_log_get_stats(hmi_alarm_log_stats_t *out);

/* --- Pure helpers (tests, --verify-log) --- */

/* Format one record line (with CRLF) chained to prev_crc. Returns the length,
 * or 0 if cap is too small. *out_crc receives this line's crc. */
size_t hmi_alarm_log_format_line(char *out, size_t cap, const hmi_alarm_record_t *rec,
                                 const char *utc_iso, uint32_t prev_crc, uint32_t *out_crc);

/* Verify one line against the running chain value *chain (updated on success).
 * Header lines set *chain from their chain= field. */
bool hmi_alarm_log_verify_line(const char *line, size_t len, uint32_t *chain);

/* Verify a whole file. Returns true if every line verifies. Counts are optional.
 * *out_first_bad receives the 1-based line number of the first failure (0 if none). */
bool hmi_alarm_log_verify_file(const char *path, uint32_t *out_lines, uint32_t *out_bad,
                               uint32_t *out_first_bad);

#endif /* INDUSTRIAL_HMI_ALARM_LOG_H */
