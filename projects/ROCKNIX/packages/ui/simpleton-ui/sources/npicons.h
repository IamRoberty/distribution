/*
 * SimpletonOS UI - the now-playing icons (0.23, 16 Oct 2026).
 *
 * Ian: "use logos whenever possible to keep down the number of
 * translations"; the selected one "grows an obvious amount and changes to
 * the selected colour" - no bubbles. So every control is a picture drawn
 * here at whatever size and colour the moment needs: a small software
 * rasteriser (4x4 samples a pixel, so the edges are smooth) into an ARGB
 * bitmap, cached by kind, size and colour. No font carries these shapes
 * at two sizes in a cover's colours; drawing them does.
 */
#ifndef SIMPLETON_NPICONS_H
#define SIMPLETON_NPICONS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NPI_PLAY, NPI_PAUSE, NPI_PREV, NPI_NEXT,
    NPI_HEART, NPI_HEART_FILLED,
    NPI_LIST,                   /* the track list (three lines) */
    NPI_GEAR,                   /* more */
    NPI_THROUGH,                /* play through: an arrow onward */
    NPI_REPEAT,                 /* repeat the album: a loop */
    NPI_REPEAT_ONE,             /* repeat the track: the loop with a 1 */
    NPI_SHUFFLE,                /* random: crossed arrows */
    NPI_COUNT
} npicon_t;

/* A bitmap `size` px square of the icon in `colour` (RRGGBB), cached; NULL
 * when out of memory. The pointer stays valid until the cache turns over
 * (dozens of other icons later), so apply it to an image object at once. */
const lv_image_dsc_t * npicon_dsc(npicon_t kind, int size, uint32_t colour);

#endif
