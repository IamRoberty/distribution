/*
 * SimpletonOS UI - screen-size-aware layout (0.11, 4 Oct 2026).
 *
 * Engineering Note 05, build step 2: sizes and positions are worked out from
 * the real screen and a row count, never from a fixed panel size.
 *
 *   ui_screen        the output as it is: width, height, handheld or TV, and
 *                    the centred square "stage" (side = the shorter edge)
 *                    that the shared screens - now-playing, dialogs - sit in.
 *
 *   ui_size_t        the "clothing sizes" of Note 05. A size is a ROW COUNT;
 *                    everything else follows from the screen:
 *
 *                      grid      XXL  XL   L    M    S
 *                      handheld  1x1  2x2  3x3  4x4  -      (S: covers too small)
 *                      TV 16:9   1x2  2x4  3x6  4x8  5x10
 *
 *                    In a list, a size is how many rows would fill the stage
 *                    top to bottom: text, row height and thumbnail follow.
 *
 *   ui_grid_metrics  rows -> columns, cover size and position on the screen,
 *                    plus the TV's info band.
 *   ui_list_metrics  rows -> row height, text size, how many rows show.
 *
 * Shared screens keep measuring in stage units through PX(): one unit is
 * 1/720 of the stage side, a proportion like a typographic point, so the
 * square stage is the same composition on every screen.
 */
#ifndef SIMPLETON_LAYOUT_H
#define SIMPLETON_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { UI_SCREEN_HANDHELD, UI_SCREEN_TV } ui_screen_kind_t;

typedef struct {
    int w, h;                 /* output resolution, real pixels             */
    int stage;                /* side of the centred square stage           */
    int stage_x, stage_y;     /* its top-left corner on the screen          */
    ui_screen_kind_t kind;    /* TV = an external display (HDMI)            */
} ui_screen_t;

extern ui_screen_t ui_screen;

/* Set once by main.c, right after the display is up and before any font or
 * screen is created. */
void layout_init(int w, int h, bool external);

/* "handheld" / "tv": the key view settings are remembered under, since the
 * TV and the handheld keep separate choices (Note 05 section 3). */
const char * ui_screen_kind_name(void);

/* Stage units -> real pixels (see the header comment). */
#define UI_STAGE_UNITS     720
#define PX(v)              ((int32_t)(((v) * ui_screen.stage + UI_STAGE_UNITS / 2) / UI_STAGE_UNITS))

/* ---- sizes ---- */

typedef enum { UI_SIZE_XXL, UI_SIZE_XL, UI_SIZE_L, UI_SIZE_M, UI_SIZE_S, UI_SIZE_COUNT } ui_size_t;

#define UI_SIZE_DEFAULT    UI_SIZE_L            /* 3x3 handheld, 3x6 TV (Ian) */

/* "XXL" ... "S". Not translated: clothing sizes read the same everywhere. */
const char * ui_size_name(ui_size_t size);

/* Whether this screen offers the size (no S on the handheld). */
bool ui_size_offered(ui_size_t size);

/* ---- album grid ---- */

typedef struct {
    int rows, cols;
    int cell;                 /* one grid step: a cover plus its share of the gap  */
    int tile;                 /* the cover's side                                  */
    int tile_focus;           /* the selected cover, "come forward" (~115%)        */
    int x, y;                 /* top-left of the grid on the SCREEN                */
    int w, h;                 /* the grid's size: cols * cell, rows * cell         */
    int band_y, band_h;       /* TV info band ("Artist - Album"); band_h 0 = none  */
} ui_grid_t;

/* Fill `g` for a size. Returns false (and fills the nearest offered size)
 * when this screen doesn't offer `size`. */
bool ui_grid_metrics(ui_size_t size, ui_grid_t * g);

/* Top-left of the cover in (row, col), on the screen. */
void ui_grid_tile_pos(const ui_grid_t * g, int row, int col, int * x, int * y);

/* ---- lists ---- */

typedef struct {
    int margin;               /* side margin of the stage                          */
    int header_h, footer_h;   /* title line above, hint line below                 */
    int chrome_font_px;       /* text size of those two lines                      */
    int x, y, w, h;           /* the list area, on the STAGE                       */
    int row_h, row_gap;
    int row_radius;
    int font_px;              /* row text size, real pixels                        */
    int thumb;                /* side of a thumbnail inside a row                  */
    int rows;                 /* whole rows that show: the paging step             */
} ui_list_t;

/* Fill `l` for a size (falls back to the nearest offered one). */
void ui_list_metrics(ui_size_t size, ui_list_t * l);

#endif
