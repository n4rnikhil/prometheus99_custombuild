/**
 * @file tags.c
 * @brief Industrial Tag Engine Implementation
 */

#include "../../include/core/tags.h"
#include <string.h>

static const hmi_tag_meta_t *s_meta_registry = NULL;
static uint8_t s_meta_count = 0;

void hmi_tags_init(hmi_tag_table_t *table)
{
    if (!table) return;
    memset(table, 0, sizeof(hmi_tag_table_t));
    table->global_dirty_mask = 0xFFFF; /* Trigger initial paint of all bands */
}

void hmi_tag_register_meta(const hmi_tag_meta_t *meta_list, uint8_t count)
{
    s_meta_registry = meta_list;
    s_meta_count = count;
}

const hmi_tag_meta_t* hmi_tag_get_meta(uint8_t tag_id)
{
    if (!s_meta_registry) return NULL;
    for (uint8_t i = 0; i < s_meta_count; i++) {
        if (s_meta_registry[i].tag_id == tag_id) {
            return &s_meta_registry[i];
        }
    }
    return NULL;
}

void hmi_tag_write_analog(hmi_tag_table_t *table, uint8_t tag_id, int16_t val, uint8_t quality)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return;
    hmi_tag_t *t = &table->tags[tag_id];
    if (t->value != val || t->quality != quality) {
        t->value = val;
        t->quality = quality;
        table->global_dirty_mask |= t->dirty_bands;
    }
}

void hmi_tag_write_digital(hmi_tag_table_t *table, uint8_t tag_id, bool state, uint8_t quality)
{
    hmi_tag_write_analog(table, tag_id, state ? 1 : 0, quality);
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

void hmi_tag_set_band_affinity(hmi_tag_table_t *table, uint8_t tag_id, uint16_t band_mask)
{
    if (!table || tag_id >= HMI_MAX_TAGS) return;
    table->tags[tag_id].dirty_bands |= band_mask;
}

uint16_t hmi_tags_consume_dirty_mask(hmi_tag_table_t *table)
{
    if (!table) return 0;
    uint16_t mask = table->global_dirty_mask;
    table->global_dirty_mask = 0;
    return mask;
}
