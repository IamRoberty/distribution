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
 *   tiletextmin 0.12      the smallest a title may be set, as a fraction of
 *                         the tile side; below it the title is trimmed with
 *                         dots instead (default 0.12, never under 16 px)
 *   tileway bg=RRGGBB ink=RRGGBB ; name      extra tile-only colourways
 *   coverfont <file.ttf> caps|mixed [leading=N] [share=N] [max=N] [min=F]
 *             [contrast=R] [long] [big]        a face the album title may be
 *                 set in (0.16i, from Ian's type notes of 9 Oct):
 *       caps|mixed  set in capitals, or as tagged
 *       leading     line spacing as a % of the face's own (default 100)
 *       share       how many cards of it go in the deck (default 10)
 *       max         only titles of up to N letters (wide faces)
 *       min         only when the title fits at this fraction of the tile
 *                   side or bigger (faces that must be large)
 *       contrast    only on a tile whose ink reads at this ratio or better
 *       long        preferred for long titles (condensed faces)
 *       big         start the size ladder a step bigger (short titles can
 *                   fill the tile)
 *
 * No coverfont lines: the old trio (the tape's album line, its mixed-case
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

#define TILE_FONT_NONE 255

typedef struct {
    uint32_t bg, ink;      /* RRGGBB */
    uint8_t  font;         /* index into tilestyle_font(), TILE_FONT_NONE = the theme's trio */
} tile_style_t;

typedef struct {
    char  file[64];
    bool  caps, for_long, big;
    int   leading;         /* percent of the face's own line height */
    int   share;
    int   max_letters;     /* 0 = any */
    float min_size;        /* 0 = any; else the fitted size must be >= this * tile side */
    float contrast;        /* 0 = any */
} tile_font_t;

/* Read the active theme's tile lines (call once the theme is known; again
 * after a theme change is harmless). `theme_path`: the .theme file. */
void tilestyle_load(const char * theme_path);

tile_colours_t tilestyle_mode(void);
const char *   tilestyle_mode_name(void);
float               tilestyle_text_min(void);     /* tiletextmin: smallest title size as a fraction of the tile */
int                 tilestyle_font_count(void);
const tile_font_t * tilestyle_font(int i);
/* The letter-count and contrast rules of face i against a title and a tile
 * (the size rule needs the fitted size, so the grid checks that one). */
bool                tilestyle_font_fits(int i, int letters, uint32_t bg, uint32_t ink);

/* Deal styles for `n` entries laid out `cols` across. `noart[e]` marks the
 * entries that need one (others are left zero); `letters[e]` is the title's
 * letter count (may be NULL). Same seed, same inputs -> same result. */
void tilestyle_deal(unsigned seed, int n, int cols, const bool * noart, const int * letters, tile_style_t * out);

/* WCAG contrast ratio of two RRGGBB colours (1..21). */
double tilestyle_contrast(uint32_t a, uint32_t b);

#endif
