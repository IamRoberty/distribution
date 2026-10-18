/*
 * SimpletonOS UI - the text of a no-art tile (0.17, 10 Oct 2026).
 *
 * The grid's artless covers (0.16b-0.16l): the album title centred in a
 * square, in the cover font the theme dealt it, whole words only, shrinking
 * down the size ladder rather than breaking a word; a bracketed tail set
 * smaller underneath; the artist above it when the tile is big enough.
 * One function, so the grid's tiles and the album page's cover square (the
 * "XXL tile" when an album has no art) are the same thing at any size.
 */
#ifndef SIMPLETON_TILETEXT_H
#define SIMPLETON_TILETEXT_H

#include "lvgl.h"
#include "tilestyle.h"
#include <stdbool.h>

typedef struct {
    lv_obj_t * box;                       /* a `side` square inside the parent, the lines centred in it */
    lv_obj_t * artist, * title, * extra;  /* its lines (extra: the bracketed tail); NULL when absent */
    int        px, lines;                 /* the title's size and line count, for logs */
    const char * file;                    /* the face it ended up in */
    bool       fallback;                  /* no face took it near the floor */
} tiletext_t;

/* Lay `title` (and `artist`, on a tile of 260 px or more) into `parent`, a
 * `side` square already coloured. `st` is the dealt style (its font and
 * colours decide the face); NULL = the theme's trio. */
void tiletext_make(lv_obj_t * parent, int side, const char * title, const char * artist, const tile_style_t * st, tiletext_t * out);
/* The same with the title's line limit given (0 = the tile's own: 5 on a big tile, 4 on a small). */
void tiletext_make_lines(lv_obj_t * parent, int side, const char * title, const char * artist, const tile_style_t * st, int title_lines, tiletext_t * out);

#endif
