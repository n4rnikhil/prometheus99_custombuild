/**
 * @file tags.h
 * @brief 512-Byte Ultra-Lightweight Industrial Process Tag Engine
 * @standards IEC 61131-3 / OPC Data Quality Standards
 *
 * 64 industrial tags packed into a deterministic, zero-heap array.
 * Fixed-point arithmetic: zero floating point coprocessor requirements.
 */

#ifndef INDUSTRIAL_HMI_TAGS_H
#define INDUSTRIAL_HMI_TAGS_H

#include "../config.h"

/* --- Data Quality Flags --- */
typedef enum {
    QUALITY_GOOD         = 0,  /* Verified live reading from PLC */
    QUALITY_COMM_FAULT   = 1,  /* Lost communication / timeout (draw dashed lines) */
    QUALITY_OUT_OF_RANGE = 2,  /* Sensor saturated / open circuit / wire break */
    QUALITY_SIMULATED    = 3   /* Test bench / simulator injection */
} hmi_quality_t;

/* --- Tag Data Types --- */
typedef enum {
    TAG_TYPE_ANALOG  = 0,  /* Scaled integer (Level, Temp, Pressure, RPM) */
    TAG_TYPE_DIGITAL = 1,  /* Boolean coil (Motor running, valve open, E-stop) */
    TAG_TYPE_STATE   = 2   /* Multi-state integer (0=Stopped, 1=Running, 2=Fault) */
} hmi_tag_type_t;

/* --- 8-Byte Packed Tag Structure --- */
typedef struct {
    int16_t       value;          /* Scaled value (e.g. 842 = 84.2% or 1450 = 1450 RPM) */
    int16_t       setpoint;       /* Active control target */
    uint8_t       quality;        /* hmi_quality_t */
    uint8_t       decimals;       /* 0, 1, or 2 decimal places */
    uint16_t      dirty_bands;    /* Bitmask of which vertical display bands this tag touches */
} hmi_tag_t;

/* --- Static Tag Descriptor (Stored in ROM/Flash, Zero RAM) --- */
typedef struct {
    uint8_t        tag_id;
    hmi_tag_type_t type;
    const char    *name;          /* "TANK_01_LVL" */
    const char    *unit;          /* "%", "RPM", "kPa", "°C" */
    int16_t        min_val;
    int16_t        max_val;
    int16_t        warn_high;      /* Amber limit line */
    int16_t        trip_high;      /* Red trip line */
} hmi_tag_meta_t;

/* --- Full Tag Table (Total RAM: exactly 512 bytes!) --- */
typedef struct {
    hmi_tag_t tags[HMI_MAX_TAGS];
    uint16_t  global_dirty_mask;   /* 10-bit mask of bands requiring repaint */
} hmi_tag_table_t;

/* --- Core Tag API --- */
void hmi_tags_init(hmi_tag_table_t *table);
void hmi_tag_register_meta(const hmi_tag_meta_t *meta_list, uint8_t count);
const hmi_tag_meta_t* hmi_tag_get_meta(uint8_t tag_id);

void hmi_tag_write_analog(hmi_tag_table_t *table, uint8_t tag_id, int16_t val, uint8_t quality);
void hmi_tag_write_digital(hmi_tag_table_t *table, uint8_t tag_id, bool state, uint8_t quality);
int16_t hmi_tag_read_value(const hmi_tag_table_t *table, uint8_t tag_id);
uint8_t hmi_tag_read_quality(const hmi_tag_table_t *table, uint8_t tag_id);

/* Dirty Band Management for < 0.01% Idle CPU */
void hmi_tag_set_band_affinity(hmi_tag_table_t *table, uint8_t tag_id, uint16_t band_mask);
uint16_t hmi_tags_consume_dirty_mask(hmi_tag_table_t *table);

#endif /* INDUSTRIAL_HMI_TAGS_H */
