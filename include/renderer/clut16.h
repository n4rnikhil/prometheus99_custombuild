/**
 * @file clut16.h
 * @brief 16-colour palettes (0xRRGGBB) for the 4-bpp band buffer
 * @standards ISA-101: grey process graphics, colour reserved for abnormal states
 *
 * Indices 0,1,13,2,5,14,4,3,15 form a grey ramp so that anti-aliased text and
 * shapes can blend through real intermediate tones. Colour hues follow the
 * Apple system palette (dark / light variants).
 */

#ifndef INDUSTRIAL_HMI_CLUT16_H
#define INDUSTRIAL_HMI_CLUT16_H

#include "../config.h"

/* Dark theme (control room) */
static const uint32_t g_hmi_palette_dark[16] = {
    [COLOR_OBSIDIAN_BG]    = 0x0B0D11,  /* window background */
    [COLOR_TITANIUM_CARD]  = 0x16191F,  /* card */
    [COLOR_CARD_BORDER]    = 0x2A2E37,  /* hairline / separators */
    [COLOR_CHALK_WHITE]    = 0xF2F3F5,  /* primary text */
    [COLOR_SILVER_MUTED]   = 0x8E929B,  /* secondary text */
    [COLOR_DARK_GRAY]      = 0x353A44,  /* bar track / inactive */
    [COLOR_CYAN_ACCENT]    = 0x64D2FF,  /* process value accent (teal) */
    [COLOR_BLUE_ACCENT]    = 0x0A84FF,  /* interactive / selection */
    [COLOR_EMERALD_OK]     = 0x30D158,  /* running / normal */
    [COLOR_AMBER_WARN]     = 0xFF9F0A,  /* warning (high) */
    [COLOR_CRIMSON_TRIP]   = 0xFF453A,  /* critical / trip / E-stop */
    [COLOR_VIBRANT_ORANGE] = 0xFFD60A,  /* setpoint / limit markers */
    [COLOR_PURPLE_SECURE]  = 0xBF5AF2,  /* security / role */
    [COLOR_SURFACE_HOVER]  = 0x20242C,  /* raised control surface */
    [COLOR_DIM_TEXT]       = 0x5C616B,  /* tertiary text, disabled */
    [COLOR_WHITE_PURE]     = 0xFFFFFF   /* text on coloured fills */
};

/* Light theme (outdoor / high ambient light) */
static const uint32_t g_hmi_palette_contrast[16] = {
    [COLOR_OBSIDIAN_BG]    = 0xE9EBEF,
    [COLOR_TITANIUM_CARD]  = 0xFFFFFF,
    [COLOR_CARD_BORDER]    = 0xC9CCD2,
    [COLOR_CHALK_WHITE]    = 0x0A0A0C,
    [COLOR_SILVER_MUTED]   = 0x464A52,
    [COLOR_DARK_GRAY]      = 0xD5D8DD,
    [COLOR_CYAN_ACCENT]    = 0x006E99,
    [COLOR_BLUE_ACCENT]    = 0x0058D0,
    [COLOR_EMERALD_OK]     = 0x1E7E34,
    [COLOR_AMBER_WARN]     = 0xB94700,
    [COLOR_CRIMSON_TRIP]   = 0xC8102E,
    [COLOR_VIBRANT_ORANGE] = 0x8A5A00,
    [COLOR_PURPLE_SECURE]  = 0x7B2FA8,
    [COLOR_SURFACE_HOVER]  = 0xF1F2F5,
    [COLOR_DIM_TEXT]       = 0x8D9199,
    [COLOR_WHITE_PURE]     = 0xFFFFFF
};

#endif /* INDUSTRIAL_HMI_CLUT16_H */
