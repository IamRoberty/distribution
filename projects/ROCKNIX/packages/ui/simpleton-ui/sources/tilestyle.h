/*
 * SimpletonOS UI - the look of the grid's no-art tiles (0.16f, 9 Oct 2026).
 *
 * Ian, 9 Oct: more variety in the artless covers - more of the theme's
 * fonts, more colour profiles, or a random colour-profile maker. All three,
 * driven by the theme file so the designer keeps control:
 *
 *   tilepalette <name> RRGGBB RRGGBB ...     a colour set (Ian's, 9 Oct)
 *   tilecolours theme|palettes|mix|generate
 *       theme     the theme's own colourways (label colour + ink), plus any
 *                 tileway lines - the pre-0.16f behaviour
 *       palettes  any colour of any tilepalette as the tile; palette inks
 *                 from the same set
 *       mix       any palette colour as the tile; palette inks from any set
 *       generate  tile colours invented each visit around the sets (hues
 *                 nudged, saturation and lightness inside their range),
 *                 inked as mix
 *   tilecontrast 2.0      the least contrast an ink may have (default 3.5;
 *                         Archer Ave. is about vibe more than contrast)
 *   tilecream FAEFD0      a cream every non-pastel tile may take as its ink
 *   tileinks palette=60 cream=25 dark=15
 *                         how often each kind of ink is drawn: a palette
 *                         colour, the cream, an off-black (a palette colour
 *                         darker than luminance 0.05); a kind a tile can't
 *                         take (nothing reads) gives its share to the others
 *   tileway bg=RRGGBB ink=RRGGBB ; name      extra tile-only colourways
 *   tilefont <file.ttf> caps|mixed           faces the titles may use
 *
 * No tilefont lines: the old trio (the tape's album line, its mixed-case
 * line, Caprasimo). No tilecolours line: theme.
 *
 * Dealing: per visit, from a shuffled deck, so a screenful of no-art tiles
 * uses each colour once before any repeats; a card that matches (or is too
 * close to) the no-art tile on the left or above is skipped for the next.
 * Fonts the same way: never the same face as the tile left or above.
 *
 * No LVGL here: plain C, testable on its own.
 */
#ifndef SIMPLETON_TILESTYLE_H
#define SIMPLETON_TILESTYLE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { TILE_COLOURS_THEME, TILE_COLOURS_PALETTES, TILE_COLOURS_MIX, TILE_COLOURS_GENERATE } tile_colours_t;

typedef struct {
    uint32_t bg, ink;      /* RRGGBB */
    uint8_t  font;         /* index into tilestyle_font() */
} tile_style_t;

/* Read the active theme's tile lines (call once the theme is known; again
 * after a theme change is harmless). `theme_path`: the .theme file. */
void tilestyle_load(const char * theme_path);

tile_colours_t tilestyle_mode(void);
const char *   tilestyle_mode_name(void);
int            tilestyle_font_count(void);
const char *   tilestyle_font(int i, bool * caps);

/* Deal styles for `n` entries laid out `cols` across. `noart[e]` marks the
 * entries that need one (others are left zero). Same seed, same inputs ->
 * same result. */
void tilestyle_deal(unsigned seed, int n, int cols, const bool * noart, tile_style_t * out);

/* WCAG contrast ratio of two RRGGBB colours (1..21). */
double tilestyle_contrast(uint32_t a, uint32_t b);

#endif
