/*
 * SimpletonOS - colours from a cover (10 Oct 2026, for the album page).
 *
 * The cache tool extracts a small palette per album from its thumbnail and
 * stores it in library.idx; the UI turns it into roles for a page at open
 * time, so a theme's contrast floor and colour mode apply without a cache
 * rebuild. No LVGL here: plain C, shared by both binaries.
 *
 * Extraction (palette_extract): median cut to 32 boxes on a sample of the
 * pixels; the 8 most frequent colours kept in order, plus the cover's own
 * extremes among colours with a real share (>= 0.15% of the pixels): the
 * brightest, the darkest, the most saturated, and the most saturated far
 * in hue from the dominant colour. The sleeve's lettering is usually one of
 * these (Ian, 10 Oct).
 *
 * Roles (palette_roles), after Ian's two looks on the unit (10 Oct):
 *   bg    the cover's dominant colour carried over, the colourful one when
 *         colour has a real presence (a colourful dominant of 12 % or more,
 *         or colourful dominants adding up to 25 %): among those, by share,
 *         colourfulness, lightness (the bright version) and whether one of
 *         the cover's own extremes reads on it. Otherwise (a dark, grey or
 *         white cover) the same score over every dominant, so the page is
 *         as dark or as pale as the cover. Paper mode: the cover's own
 *         near-white or near-black when it has one.
 *   ink   of the cover's brightest and darkest the one that reads better
 *         (at 2.5 or more); else cream or off-black. The most legible
 *         colour: the SELECTED track, the names, the numbers.
 *   text  a colourful colour of the cover that reads on bg (by presence
 *         and colourfulness): the unselected tracks - the vibe. 0 = none
 *         (the page uses the ink).
 *   play  another colourful colour, apart from the text; 0 = none (the
 *         page borrows one of the theme's).
 *   sel   = ink.
 */
#ifndef SIMPLETON_PALETTE_H
#define SIMPLETON_PALETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAL_DOM 8

typedef struct {
    uint32_t dom[PAL_DOM];     /* RRGGBB, most frequent first; 0 past ndom */
    uint8_t  share[PAL_DOM];   /* percent of the cover each covers (0.17a: a "big block of colour" is a share) */
    uint8_t  ndom;
    uint32_t bright, dark, accent, accent2;
} art_palette_t;

typedef struct { uint32_t bg, ink, text, play, sel; } art_roles_t;   /* text / play 0 = none found */

typedef enum { PAL_MODE_DOMINANT, PAL_MODE_PAPER } pal_mode_t;

/* XRGB8888 pixels (as art_decode_fit returns), any size; samples at most
 * ~16k pixels. Returns false (and a zeroed palette) on a degenerate image. */
bool palette_extract(const uint8_t * xrgb, int w, int h, art_palette_t * out);

/* Pick the page roles. `min_contrast` is the theme's floor (for the
 * extremes' readability test on a candidate background), `cream` and
 * `dark_ink` the ink fallbacks; `theme_accent` is unused since 0.17b (the
 * page borrows from the theme itself). Returns false when the palette is
 * empty. */
bool palette_roles(const art_palette_t * p, pal_mode_t mode, float min_contrast,
                   uint32_t cream, uint32_t dark_ink, uint32_t theme_accent, art_roles_t * out);

double palette_contrast(uint32_t a, uint32_t b);      /* WCAG ratio, 1..21 */

/* The index line: 21 fields "dom0 ... dom7 ndom bright dark accent accent2
 * share0 ... share7" (hex colours, decimal shares). parse takes the 13-field
 * form of 0.17 too (shares then unknown: 0). */
#define PAL_FIELDS 21
/* A palette written by 0.17 has colours but no shares: worth re-extracting. */
static inline bool palette_complete(const art_palette_t * p)
{
    if(!p || p->ndom == 0) return false;
    for(int i = 0; i < p->ndom; i++) if(p->share[i]) return true;
    return false;
}
void palette_format(const art_palette_t * p, char * out, size_t len);
bool palette_parse(const char * const * fields, int n, art_palette_t * out);

#endif
