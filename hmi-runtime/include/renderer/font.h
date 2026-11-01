/**
 * @file font.h
 * @brief Compiled-in 2-bit anti-aliased bitmap fonts (Inter, SIL OFL 1.1)
 *
 * Glyph bitmaps are 2 bits per pixel (coverage 0..3), packed 4 pixels per
 * byte MSB-first, each row padded to a whole byte. No font engine, no files,
 * no heap: the renderer blits these straight into the band buffer.
 */

#ifndef INDUSTRIAL_HMI_FONT_H
#define INDUSTRIAL_HMI_FONT_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t  w, h;        /* bitmap size */
    int8_t   xoff;        /* from pen x */
    int8_t   yoff;        /* from top of the line box */
    uint8_t  adv;         /* pen advance */
    uint16_t off;         /* byte offset into bits */
} hmi_glyph_t;

typedef struct {
    uint8_t  line_h;      /* line box height (ascent + descent) */
    uint8_t  ascent;
    uint8_t  first;       /* first char code when map == NULL (contiguous ASCII) */
    uint8_t  count;
    const char        *map;     /* non-NULL: chars in glyph order (subset fonts) */
    const hmi_glyph_t *glyphs;
    const uint8_t     *bits;
} hmi_font_t;

extern const hmi_font_t hmi_font_caption;   /* Inter SemiBold 11 px - labels, chips */
extern const hmi_font_t hmi_font_body;      /* Inter Regular 13 px  - body text */
extern const hmi_font_t hmi_font_body_b;    /* Inter SemiBold 13 px - emphasised body, buttons */
extern const hmi_font_t hmi_font_title;     /* Inter SemiBold 16 px - card titles */
extern const hmi_font_t hmi_font_headline;  /* Inter SemiBold 22 px - screen titles */
extern const hmi_font_t hmi_font_hero_s;    /* Inter SemiBold 30 px - digits only, tabular */
extern const hmi_font_t hmi_font_hero_m;    /* Inter SemiBold 46 px - digits only, tabular */
extern const hmi_font_t hmi_font_hero_l;    /* Inter SemiBold 66 px - digits only, tabular */

/* Glyph for c, or NULL if the font has no such glyph. */
static inline const hmi_glyph_t* hmi_font_glyph(const hmi_font_t *f, char c)
{
    if (!f) return NULL;
    if (!f->map) {
        unsigned idx = (unsigned)(unsigned char)c - f->first;
        return (idx < f->count) ? &f->glyphs[idx] : NULL;
    }
    for (unsigned i = 0; i < f->count; i++) {
        if (f->map[i] == c) return &f->glyphs[i];
    }
    return NULL;
}

#endif /* INDUSTRIAL_HMI_FONT_H */
