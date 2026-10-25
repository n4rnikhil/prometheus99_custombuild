/**
 * @file tags.h
 * @brief Static process tag table (64 x 8-byte tags) with a 60 s trend buffer
 * @standards IEC 61131-3 data model / OPC-style quality flags
 *
 * Values are scaled integers. A tag's metadata says how to display it:
 * raw 842 with decimals = 1 is shown as "84.2". Limits (warn/trip) are in the
 * same raw units as the value.
 */

#ifndef INDUSTRIAL_HMI_TAGS_H
#define INDUSTRIAL_HMI_TAGS_H

#include "../config.h"

/* --- Data quality --- */
typedef enum {
    QUALITY_GOOD         = 0,  /* live reading */
    QUALITY_COMM_FAULT   = 1,  /* lost communication / timeout */
    QUALITY_OUT_OF_RANGE = 2,  /* sensor saturated / wire break */
    QUALITY_SIMULATED    = 3   /* simulator / test injection */
} hmi_quality_t;

/* --- Tag kinds --- */
typedef enum {
    TAG_TYPE_ANALOG  = 0,
    TAG_TYPE_DIGITAL = 1,
    TAG_TYPE_STATE   = 2
} hmi_tag_type_t;

/* --- 8-byte runtime tag --- */
typedef struct {
    int16_t       value;
    int16_t       setpoint;
    uint8_t       quality;       /* hmi_quality_t */
    uint8_t       decimals;      /* copied from metadata by hmi_tags_init_from_meta */
    uint16_t      dirty_bands;   /* reserved */
} hmi_tag_t;

/* --- Static tag descriptor (const, lives in .rdata) ---
 * Field order matters: drivers use positional initialisers. */
typedef struct {
    uint8_t        tag_id;
    hmi_tag_type_t type;
    const char    *name;          /* "TANK 01 LEVEL" (ASCII only: 5x7 font) */
    const char    *unit;          /* "%", "RPM", "kPa", "C" */
    int16_t        min_val;       /* raw units */
    int16_t        max_val;       /* raw units */
    int16_t        warn_high;     /* raw units; <= min_val disables */
    int16_t        trip_high;     /* raw units; <= min_val disables */
    uint8_t        decimals;      /* display decimals for raw value */
    uint8_t        digital_alarm_sev; /* DIGITAL tags: hmi_alarm_sev_t raised while value != 0 (0 = none) */
    const char    *action1;       /* operator guidance line 1 on alarm (NULL = generic) */
    const char    *action2;       /* operator guidance line 2 on alarm (NULL = generic) */
} hmi_tag_meta_t;

/* --- Tag table + 60 s trend (trend is for the tank level tag) --- */
typedef struct {
    hmi_tag_t tags[HMI_MAX_TAGS];
    int16_t   trend_samples[HMI_TREND_SAMPLES];
    uint8_t   trend_head;        /* index of the next write */
    uint8_t   trend_count;       /* number of valid samples (<= HMI_TREND_SAMPLES) */
    uint16_t  global_dirty_mask; /* reserved */
} hmi_tag_table_t;

/* --- Metadata registry ---
 * Register every driver's metadata ONCE, before any worker thread starts.
 * After that the registry is read-only, so lookups need no locking.
 * Multiple registrations accumulate (indexed by tag_id, O(1) lookup). */
void hmi_tag_registry_reset(void);
bool hmi_tag_register_meta(const hmi_tag_meta_t *meta_list, uint8_t count); /* false: NULL, id >= HMI_MAX_TAGS, or duplicate id (nothing registered for that entry) */
const hmi_tag_meta_t* hmi_tag_get_meta(uint8_t tag_id);                     /* NULL if unregistered */

/* --- Table --- */
void hmi_tags_init(hmi_tag_table_t *table);   /* zero, every tag QUALITY_COMM_FAULT until first write, decimals from registry */

void hmi_tag_write_analog(hmi_tag_table_t *table, uint8_t tag_id, int16_t val, uint8_t quality);
void hmi_tag_write_digital(hmi_tag_table_t *table, uint8_t tag_id, bool state, uint8_t quality);
void hmi_tag_set_quality(hmi_tag_table_t *table, uint8_t tag_id, uint8_t quality); /* keeps value */
int16_t hmi_tag_read_value(const hmi_tag_table_t *table, uint8_t tag_id);
uint8_t hmi_tag_read_quality(const hmi_tag_table_t *table, uint8_t tag_id);

/* --- Trend --- */
#define HMI_TREND_GAP INT16_MIN   /* sample recorded while the value was not valid */
void hmi_tags_push_trend(hmi_tag_table_t *table, int16_t sample);
/* Copy the trend oldest -> newest into out[0..cap). Returns the number copied. */
uint8_t hmi_tags_copy_trend(const hmi_tag_table_t *table, int16_t *out, uint8_t cap);

/* --- Formatting helper (shared by renderer and alarm text) ---
 * Formats a scaled integer: (842, 1) -> "84.2", (-5, 1) -> "-0.5", (1450, 0) -> "1450".
 * Handles INT16_MIN. Always NUL-terminates when cap > 0. Returns chars written (excl NUL). */
int hmi_format_scaled(char *out, size_t cap, int16_t raw, uint8_t decimals);

#endif /* INDUSTRIAL_HMI_TAGS_H */
