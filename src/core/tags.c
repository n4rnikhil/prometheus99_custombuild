/**
 * @file tags.c
 * @brief Tag table, metadata registry, trend buffer and scaled-value formatting
 */

#include "../../include/core/tags.h"
#include <string.h>
#include <stdio.h>

/* O(1) registry indexed by tag id. Filled before worker threads start, read-only afterwards. */
static const hmi_tag_meta_t *s_meta_by_id[HMI_MAX_TAGS];

void hmi_tag_registry_reset(void)
{
    memset((void *)s_meta_by_id, 0, sizeof(s_meta_by_id));
}

bool hmi_tag_register_meta(const hmi_tag_meta_t *meta_list, uint8_t count)
{
    bool ok = true;
    if (!meta_list) return false;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t id = meta_list[i].tag_id;
        if (id >= HMI_MAX_TAGS || s_meta_by_id[id] != NULL) {
            ok = false;
            continue;
        }
        s_meta_by_id[id] = &meta_list[i];
    }
    return ok;
}

const hmi_tag_meta_t* hmi_tag_get_meta(uint8_t tag_id)
{
    if (tag_id >= HMI_MAX_TAGS) return NULL;
    return s_meta_by_id[tag_id];
}

void hmi_tags_init(hmi_tag_table_t *table)
{
    if (!table) return;
    memset(table, 0, sizeof(hmi_tag_table_t));
    for (uint8_t i = 0; i < HMI_MAX_TAGS; i++) {
        const hmi_tag_meta_t *m = s_meta_by_id[i];
        table->tags[i].quality  = QUALITY_COMM_FAULT;   /* no data yet */
        table->tags[i].decimals = m ? m->decimals : 0;
    }
}

void hmi_tag_write_analog(hmi_tag_table_t *table, uint8_t tag_id, int16_t val, uint8_t quality)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return;
    hmi_tag_t *t = &table->tags[tag_id];
    t->value = val;
    t->quality = quality;
}

void hmi_tag_write_digital(hmi_tag_table_t *table, uint8_t tag_id, bool state, uint8_t quality)
{
    hmi_tag_write_analog(table, tag_id, (int16_t)(state ? 1 : 0), quality);
}

void hmi_tag_set_quality(hmi_tag_table_t *table, uint8_t tag_id, uint8_t quality)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return;
    table->tags[tag_id].quality = quality;
}

int16_t hmi_tag_read_value(const hmi_tag_table_t *table, uint8_t tag_id)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return 0;
    return table->tags[tag_id].value;
}

uint8_t hmi_tag_read_quality(const hmi_tag_table_t *table, uint8_t tag_id)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return QUALITY_COMM_FAULT;
    return table->tags[tag_id].quality;
}

void hmi_tags_push_trend(hmi_tag_table_t *table, int16_t sample)
{
    if (!table) return;
    if (table->trend_head >= HMI_TREND_SAMPLES) table->trend_head = 0;
    table->trend_samples[table->trend_head] = sample;
    table->trend_head = (uint8_t)((table->trend_head + 1u) % HMI_TREND_SAMPLES);
    if (table->trend_count < HMI_TREND_SAMPLES) {
        table->trend_count++;
    }
}

uint8_t hmi_tags_copy_trend(const hmi_tag_table_t *table, int16_t *out, uint8_t cap)
{
    if (!table || !out || cap == 0) return 0;
    uint8_t count = table->trend_count;
    if (count > HMI_TREND_SAMPLES) count = HMI_TREND_SAMPLES;
    uint8_t head = (uint8_t)(table->trend_head % HMI_TREND_SAMPLES);
    /* Oldest sample: head - count (mod N) */
    uint8_t start = (uint8_t)((head + HMI_TREND_SAMPLES - count) % HMI_TREND_SAMPLES);
    uint8_t skip = (count > cap) ? (uint8_t)(count - cap) : 0;   /* keep the newest cap samples */
    uint8_t n = 0;
    for (uint8_t i = skip; i < count; i++) {
        out[n++] = table->trend_samples[(start + i) % HMI_TREND_SAMPLES];
    }
    return n;
}

int hmi_format_scaled(char *out, size_t cap, int16_t raw, uint8_t decimals)
{
    if (!out || cap == 0) return 0;
    int32_t v = raw;                       /* widen first: -INT16_MIN fits in int32 */
    bool neg = (v < 0);
    uint32_t mag = (uint32_t)(neg ? -v : v);
    int n;
    if (decimals == 0) {
        n = snprintf(out, cap, "%s%lu", neg ? "-" : "", (unsigned long)mag);
    } else {
        if (decimals > 4) decimals = 4;
        uint32_t div = 1;
        for (uint8_t i = 0; i < decimals; i++) div *= 10u;
        n = snprintf(out, cap, "%s%lu.%0*lu", neg ? "-" : "",
                     (unsigned long)(mag / div), (int)decimals, (unsigned long)(mag % div));
    }
    if (n < 0) { out[0] = '\0'; return 0; }
    if ((size_t)n >= cap) return (int)(cap - 1);
    return n;
}
