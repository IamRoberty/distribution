/*
 * SimpletonOS UI - screen-size-aware layout (implementation). See layout.h.
 *
 * Every number below is a proportion of the screen or of a row, written as
 * a fraction. Where a 0.10 screen exists, the fractions reproduce its pixels
 * exactly on the 720x720 panel and on a 1080p TV (checked by rendering both
 * versions side by side); the comment beside each gives those two values.
 */
#include "layout.h"

ui_screen_t ui_screen = { .w = 720, .h = 720, .stage = 720, .kind = UI_SCREEN_HANDHELD };

void layout_init(int w, int h, bool external)
{
    if(w < 1) w = 1;
    if(h < 1) h = 1;
    ui_screen.w = w;
    ui_screen.h = h;
    ui_screen.stage = w < h ? w : h;
    ui_screen.stage_x = (w - ui_screen.stage) / 2;
    ui_screen.stage_y = (h - ui_screen.stage) / 2;
    ui_screen.kind = external ? UI_SCREEN_TV : UI_SCREEN_HANDHELD;
}

const char * ui_screen_kind_name(void)
{
    return ui_screen.kind == UI_SCREEN_TV ? "tv" : "handheld";
}

/* ---- sizes ---- */

static const char * const size_names[UI_SIZE_COUNT] = { "XXL", "XL", "L", "M", "S" };

const char * ui_size_name(ui_size_t size)
{
    return (unsigned)size < UI_SIZE_COUNT ? size_names[size] : "";
}

bool ui_size_offered(ui_size_t size)
{
    if((unsigned)size >= UI_SIZE_COUNT) return false;
    /* 5x5 covers on the 76 mm panel would be 14 mm: no S on the handheld */
    if(size == UI_SIZE_S) return ui_screen.kind == UI_SCREEN_TV;
    return true;
}

static ui_size_t nearest_offered(ui_size_t size)
{
    if((unsigned)size >= UI_SIZE_COUNT) size = UI_SIZE_DEFAULT;
    while(size > UI_SIZE_XXL && !ui_size_offered(size)) size--;
    return size;
}

/* ---- album grid ---- */

/* Rows per size. Columns are never listed: they follow from the shape. */
static const int grid_rows[UI_SIZE_COUNT] = { 1, 2, 3, 4, 5 };

#define GRID_BAND_DIV       9       /* TV info band = screen height / 9  (120 px at 1080p)  */
#define GRID_BAND_AT_TOP    1       /* "top for now; may move" (Ian, Note 05 section 4)      */
#define GRID_GAP_PCT        4       /* gap between covers, % of a cell   [tune on the panel] */
#define GRID_FOCUS_PCT      115     /* the selected cover comes forward                      */

bool ui_grid_metrics(ui_size_t size, ui_grid_t * g)
{
    ui_size_t use = nearest_offered(size);
    int rows = grid_rows[use];

    /* The band is what makes a 16:9 screen an exact 2:1 field of squares:
     * 1080 - 120 = 960, and 1920 / 960 = 2 columns per row. */
    int band = ui_screen.kind == UI_SCREEN_TV ? ui_screen.h / GRID_BAND_DIV : 0;
    int field_h = ui_screen.h - band;

    int cell = field_h / rows;
    if(cell > ui_screen.w) cell = ui_screen.w;        /* a screen taller than wide */
    if(cell < 1) cell = 1;
    int cols = ui_screen.w / cell;
    if(cols < 1) cols = 1;

    int gap = cell * GRID_GAP_PCT / 100;
    gap += gap & 1;                                   /* even: the same on both sides */
    if(gap < 2) gap = 2;
    if(gap >= cell) gap = 0;

    g->rows = rows;
    g->cols = cols;
    g->cell = cell;
    g->tile = cell - gap;
    g->tile_focus = g->tile * GRID_FOCUS_PCT / 100;
    g->w = cols * cell;
    g->h = rows * cell;
    g->x = (ui_screen.w - g->w) / 2;
    g->band_h = band;
    g->band_y = GRID_BAND_AT_TOP ? 0 : ui_screen.h - band;
    g->y = (GRID_BAND_AT_TOP ? band : 0) + (field_h - g->h) / 2;
    return use == size;
}

void ui_grid_tile_pos(const ui_grid_t * g, int row, int col, int * x, int * y)
{
    int inset = (g->cell - g->tile) / 2;
    if(x) *x = g->x + col * g->cell + inset;
    if(y) *y = g->y + row * g->cell + inset;
}

/* ---- lists ---- */

/* How many rows would fill the stage top to bottom, per size. L is the
 * 0.10 browser: stage / 9 = 80 px on the panel, 120 px at 1080p. The other
 * four are first proposals, to be judged on the panel when the picker
 * arrives. */
static const int list_rows_per_stage[UI_SIZE_COUNT] = { 5, 7, 9, 11, 14 };

void ui_list_metrics(ui_size_t size, ui_list_t * l)
{
    ui_size_t use = nearest_offered(size);
    int s = ui_screen.stage;

    l->margin         = s / 30;                 /* 24 / 36  */
    l->header_h       = s / 10;                 /* 72 / 108 */
    l->footer_h       = s * 11 / 180;           /* 44 / 66  */
    l->chrome_font_px = s / 36;                 /* 20 / 30  */

    l->x = l->margin;
    l->y = l->header_h;
    l->w = s - 2 * l->margin;
    l->h = s - l->header_h - l->footer_h;

    l->row_h      = s / list_rows_per_stage[use];   /* L: 80 / 120 */
    if(l->row_h < 8) l->row_h = 8;
    l->row_gap    = l->row_h / 20;                  /* L: 4 / 6    */
    l->row_radius = l->row_h / 8;                   /* L: 10 / 15  */
    l->font_px    = l->row_h * 7 / 20;              /* L: 28 / 42  */
    l->thumb      = l->row_h - 2 * l->row_gap;

    l->rows = l->h / (l->row_h + l->row_gap);
    if(l->rows < 1) l->rows = 1;
}
