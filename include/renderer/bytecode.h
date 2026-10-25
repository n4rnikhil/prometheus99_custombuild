/**
 * @file bytecode.h
 * @brief 4-byte UI manifest tokens: a driver describes its own screen
 *
 * A device driver ships a const array of tokens. The renderer draws ONLY the
 * widgets listed there, so a device costs exactly the flash its manifest and
 * the widget code it uses need (12-14 tokens = 48-56 bytes per screen).
 *
 * Token layout (32 bits):
 *   bits 24..31  tag id (0..255, must be < HMI_MAX_TAGS)
 *   bits 16..23  widget type (hmi_widget_type_t)
 *   bits 12..15  grid x      (0..15)   - 16 x 16 grid over the content area
 *   bits  8..11  grid y      (0..15)
 *   bits  4..7   grid width  (1..15, 0 = 16)
 *   bits  0..3   grid height (1..15, 0 = 16)
 */

#ifndef INDUSTRIAL_HMI_BYTECODE_H
#define INDUSTRIAL_HMI_BYTECODE_H

#include "../config.h"

typedef enum {
    WIDGET_CONTAINER_CARD = 1,  /* rounded card; title + status from the tag */
    WIDGET_HERO_DIGIT     = 2,  /* large value + unit (Inter, tabular digits) */
    WIDGET_ANALOG_BAR     = 3,  /* label, value and bar with H / HH limit ticks */
    WIDGET_STATUS_PILL    = 4,  /* digital state chip (Running / Stopped) */
    WIDGET_ACTION_BUTTON  = 5,  /* touch button (>= 48 px); action depends on the tag */
    WIDGET_ACTION_BANNER  = 6,  /* reserved: banner is drawn by the frame */
    WIDGET_TREND_SPARK    = 7,  /* 60 s trend chart of the trend buffer */
    WIDGET_TANK           = 8,  /* vertical vessel with level and H / HH marks */
    WIDGET_PUMP           = 9   /* pump symbol, impeller turns while running */
} hmi_widget_type_t;

#define HMI_GRID_CELLS 16

#define HMI_TOKEN_PACK(tag_id, type, grid_x, grid_y, grid_w, grid_h) \
    ( (((uint32_t)(tag_id)  & 0xFFu) << 24) | \
      (((uint32_t)(type)    & 0xFFu) << 16) | \
      (((uint32_t)(grid_x)  & 0x0Fu) << 12) | \
      (((uint32_t)(grid_y)  & 0x0Fu) << 8)  | \
      (((uint32_t)(grid_w)  & 0x0Fu) << 4)  | \
      (((uint32_t)(grid_h)  & 0x0Fu)) )

static inline uint8_t hmi_token_get_tag(uint32_t tok)  { return (uint8_t)((tok >> 24) & 0xFFu); }
static inline uint8_t hmi_token_get_type(uint32_t tok) { return (uint8_t)((tok >> 16) & 0xFFu); }
static inline uint8_t hmi_token_grid_x(uint32_t tok)   { return (uint8_t)((tok >> 12) & 0x0Fu); }
static inline uint8_t hmi_token_grid_y(uint32_t tok)   { return (uint8_t)((tok >> 8) & 0x0Fu); }
static inline uint8_t hmi_token_grid_w(uint32_t tok)   { uint8_t v = (uint8_t)((tok >> 4) & 0x0Fu); return v ? v : 16u; }
static inline uint8_t hmi_token_grid_h(uint32_t tok)   { uint8_t v = (uint8_t)(tok & 0x0Fu); return v ? v : 16u; }

#endif /* INDUSTRIAL_HMI_BYTECODE_H */
