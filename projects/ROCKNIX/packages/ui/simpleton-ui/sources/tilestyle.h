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
 * The album page (0.17, 10 Oct 2026; album-page-6b.md) takes its layouts
 * from the same file, one per line, one drawn at random per visit among
 * those for the screen:
 *
 *   linerfont <file.ttf> caps|mixed [leading=N] [share=N]
 *                 a face the track list may be set in
 *   pagelayout <name> screen=tv|handheld [art=left|right|centre]
 *              [sides=none|letters|numbers] [header=artist-big|album-big|centred|none]
 *              [artist=<file>|any] [album=<file>|any] [liner=<file>|any]
 *              [size=S|M|L] [leading=N] [numbers=none|01|1.|1]
 *              [align=left|centre|right] [colours=art|paper|tile]
 *       art       TV: where the cover square goes; centre splits the list
 *                 in two like the sides of a record
 *       sides     the words over a split list: "Side A" / "Side B", or
 *                 "Side 1" / "Side 2" ("Disc 1" / "Disc 2" when the album
 *                 has two discs); set in the artist face
 *       header    handheld: which name is big (the TV shows no names over
 *                 art); none = both small
 *       artist, album   cover fonts for the names (their caps and leading
 *                 from the coverfont line); any = dealt per visit
 *       liner     a linerfont for the list; any = dealt per visit
 *       size      the list type: S, M or L
 *       leading   line spacing as a % of the liner face's own (default the
 *                 linerfont's)
 *       numbers   none, or hanging in a gutter before the titles
 *       colours   from the cover's main colour, from its paper (its own
 *                 white or black), or a tile colourway as on the grid
 *                 (always a tile colourway when the album has no art)
 *   accent RRGGBB  the theme's accent: the playing colour when the cover
 *                 has none of its own
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
/* "Redmean" distance of two colours, 0..~765; under 70 they read as one. */
double tilestyle_distance(uint32_t a, uint32_t b);
/* HSL saturation, 0..1. */
double tilestyle_saturation(uint32_t c);

/* The coverfont index for a file name (the page's header faces), -1 = not listed. */
int tilestyle_font_index(const char * file);

/* Theme lines the page shares with the tiles. */
double   tilestyle_min_contrast(void);
uint32_t tilestyle_cream(void);        /* 0 = none */
uint32_t tilestyle_accent(void);       /* the theme's accent line, 0 = none */

/* An off-black from the palettes (the darkest), or 0x202020. */
uint32_t tilestyle_dark_ink(void);

/* The battery indicator (0.18a, Ian's design for Archer Ave.): a solid disc
 * top right with the percentage in it, no icon, no sign. One theme line:
 *   battery diameter=50 font=Jost-Bold.ttf size=22 high=45ACC8 mid=E3D200 low=B41823
 *           charge=17B436 ink=29221E inklight=FAEFD0 [mid_from=10] [high_from=50]
 * The disc is `high` from high_from %, `mid` from mid_from %, `low` under
 * that, `charge` on the cable; the number in whichever ink reads better.
 * Missing values take Archer's. diameter and size are stage units. */
typedef struct {
    int      diameter, size;
    int      mid_from, high_from;
    char     font[64];
    uint32_t high, mid, low, charge, ink, ink_light;
} battery_style_t;
const battery_style_t * tilestyle_battery(void);

/* A second colourway from the same tilepalette set as `tile` (the album
 * page beside a no-art tile: the tile's own colours carry over, Ian 10 Oct):
 * another colour of the set as the background, inked from the set as the
 * tiles are, the same font. A set with nothing else usable gives the tile
 * inverted (its ink as the page, its colour as the text). */
void tilestyle_companion(unsigned seed, const tile_style_t * tile, tile_style_t * out);

/* The other colours of the tilepalette set `bg` belongs to (in the theme's
 * order, `bg` itself left out), up to `max` of them; 0 when `bg` is no
 * set's colour (a generated or theme colour). The album page's cassette
 * and record are tinted from these (0.17h). */
int tilestyle_set_colours(uint32_t bg, uint32_t * out, int max);

/* Colours for a designed picture in a tile's colours - the album page's
 * cassette or record (0.17h; Ian, 10 Oct: the tape in the colours of the
 * tile, starting from the same background). `label`: a colour of the
 * tile's set the tile's ink reads on (the names are written on it in the
 * ink), the tile's own colour when none does. `shell`: another colour of
 * the set, apart from the background and the label, dealt by seed; the
 * ink itself when the set has nothing else. `dark`: the set's darkest
 * colour (a record's disc), the theme's off-black when the set has no
 * dark one apart from the background. */
void tilestyle_picture_colours(unsigned seed, const tile_style_t * tile, uint32_t * label, uint32_t * shell, uint32_t * dark);

/* Two accents for a page on `bg` with text in `ink`: the playing colour (a
 * saturated palette colour that reads on bg and differs from the ink) and
 * the selected colour (another such). Seeded so a visit holds them. Either
 * falls back to the ink. */
void tilestyle_page_accents(unsigned seed, uint32_t bg, uint32_t ink, uint32_t * play, uint32_t * sel);

/* ---- the album page's layouts ---- */

typedef enum { PAGE_SCREEN_TV, PAGE_SCREEN_HANDHELD } page_screen_t;
typedef enum { PAGE_ART_LEFT, PAGE_ART_RIGHT, PAGE_ART_CENTRE } page_art_t;
typedef enum { PAGE_SIDES_NONE, PAGE_SIDES_LETTERS, PAGE_SIDES_NUMBERS } page_sides_t;
typedef enum { PAGE_HEADER_ARTIST_BIG, PAGE_HEADER_ALBUM_BIG, PAGE_HEADER_CENTRED, PAGE_HEADER_NONE } page_header_t;
typedef enum { PAGE_NUMBERS_NONE, PAGE_NUMBERS_PADDED, PAGE_NUMBERS_DOT, PAGE_NUMBERS_PLAIN } page_numbers_t;
typedef enum { PAGE_ALIGN_LEFT, PAGE_ALIGN_CENTRE, PAGE_ALIGN_RIGHT } page_align_t;
typedef enum { PAGE_COLOURS_ART, PAGE_COLOURS_PAPER, PAGE_COLOURS_TILE } page_colours_t;

typedef struct {
    char           name[32];
    page_screen_t  screen;
    page_art_t     art;
    page_sides_t   sides;
    page_header_t  header;
    char           artist[64], album[64], liner[64];   /* font files; "" = any (dealt) */
    char           size;                               /* 'S', 'M', 'L' */
    int            leading;                            /* 0 = the liner face's own */
    page_numbers_t numbers;
    page_align_t   align;
    page_colours_t colours;
} page_layout_t;

int                  tilestyle_layout_count(void);
const page_layout_t * tilestyle_layout(int i);
/* One layout for the screen, drawn from a deck per visit (NULL when the
 * theme lists none for it). */
const page_layout_t * tilestyle_layout_pick(unsigned seed, page_screen_t screen);

int                  tilestyle_liner_count(void);
const tile_font_t *  tilestyle_liner(int i);
int                  tilestyle_liner_index(const char * file);   /* -1 = not listed */
int                  tilestyle_liner_pick(unsigned seed);         /* by share; -1 = none listed */

#endif
