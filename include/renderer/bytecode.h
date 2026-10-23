/**
 * @file bytecode.h
 * @brief 4-Byte HMI Micro-Bytecode Token Engine
 * @standards Extreme Embedded Footprint: Complete screens in 40 to 64 bytes of Flash!
 *
 * Each widget is encoded as a single 32-bit token:
 * [ Tag ID: 8b ] [ Widget Type: 8b ] [ X: 4b | Y: 4b | W: 4b | H: 4b ]
 */

#ifndef INDUSTRIAL_HMI_BYTECODE_H
#define INDUSTRIAL_HMI_BYTECODE_H

#include "../config.h"

/* --- Bytecode Widget Types --- */
typedef enum {
    WIDGET_CONTAINER_CARD  = 1, /* Apple HIG elevated slate card */
    WIDGET_HERO_DIGIT       = 2, /* Procedural 7-segment big bold LED value */
    WIDGET_ANALOG_BAR       = 3, /* High-performance bar with HI/LO limit ticks */
    WIDGET_STATUS_PILL      = 4, /* Rounded badge (● NORMAL, ▲ WARN, ■ CRITICAL) */
    WIDGET_ACTION_BUTTON    = 5, /* Touch-friendly button (≥ 48px OSHA minimum) */
    WIDGET_ACTION_BANNER    = 6, /* Calm Emergency Action guidance banner */
    WIDGET_TREND_SPARK      = 7  /* Micro 60-second real-time trend line */
} hmi_widget_type_t;

/* --- Macro to Pack 4-Byte Token --- */
#define HMI_TOKEN_PACK(tag_id, type, grid_x, grid_y, grid_w, grid_h) \
    ( (((uint32_t)(tag_id)  & 0xFF) << 24) | \
      (((uint32_t)(type)    & 0xFF) << 16) | \
      (((uint32_t)(grid_x)  & 0x0F) << 12) | \
      (((uint32_t)(grid_y)  & 0x0F) << 8)  | \
      (((uint32_t)(grid_w)  & 0x0F) << 4)  | \
      (((uint32_t)(grid_h)  & 0x0F)) )

/* --- Unpack Helper Functions --- */
static inline uint8_t  hmi_token_get_tag(uint32_t tok)  { return (uint8_t)((tok >> 24) & 0xFF); }
static inline uint8_t  hmi_token_get_type(uint32_t tok) { return (uint8_t)((tok >> 16) & 0xFF); }
static inline uint16_t hmi_token_get_norm_x(uint32_t tok) { return (uint16_t)(((tok >> 12) & 0x0F) * 62 + 5); }
static inline uint16_t hmi_token_get_norm_y(uint32_t tok) { return (uint16_t)(((tok >> 8)  & 0x0F) * 62 + 5); }
static inline uint16_t hmi_token_get_norm_w(uint32_t tok) { 
    uint8_t gw = (uint8_t)((tok >> 4) & 0x0F);
    return (uint16_t)((gw == 0 ? 16 : gw) * 62); 
}
static inline uint16_t hmi_token_get_norm_h(uint32_t tok) { 
    uint8_t gh = (uint8_t)(tok & 0x0F);
    return (uint16_t)((gh == 0 ? 16 : gh) * 62); 
}

#endif /* INDUSTRIAL_HMI_BYTECODE_H */
