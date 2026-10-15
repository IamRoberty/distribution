/*
 * SimpletonOS UI - the Artists rows view (implementation). See rows.h.
 *
 * Shape (0.20a, Ian 14 Oct: "give an artist multiple rows if they need
 * it" - no sliding): the field is a column of lines, one grid cell tall;
 * an artist takes as many lines as their albums need at the screen's
 * width (slots = the grid's columns less one), the artist's tile spanning
 * those lines at the left, the albums running right and wrapping. Only the
 * artists whose lines are on screen (and one line either side) exist as
 * objects, and of those only the album tiles on those lines.
 *   band     (TV) the name line at the top
 *   field    scrolls over total_lines * g.cell
 *   namebar  (handheld) the momentary "Artist - Album" subtitle
 * and the shared picker over the field.
 *
 * Covers and the artists' pictures come from thumbs.c as the grid's do,
 * kept in a small bitmap cache keyed by (artist, album, px).
 */
#include "rows.h"
#include "artists.h"
#include "cache.h"
#include "config.h"
#include "libs.h"
#include "picker.h"
#include "power.h"
#include "strings.h"
#include "theme.h"
#include "thumbs.h"
#include "tiletext.h"
#include "textfit.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_ROWOBJ    14
#define MAX_TILES     160
#define BMP_MAX       256
#define BMP_BUDGET    (24u * 1024u * 1024u)
#define ROWS_BASE     (2 << 20)              /* thumbs ids of ours */
#define LINE_MS       110
#define PAGE_MS       160
#define ZOOM_MS       100
#define NAMEBAR_MS    1400
#define STRIP_MIN_ROWS 3

/* ---- options ---- */

enum { OPENS_ALBUMS, OPENS_TRACKS, OPENS_TRACKS_ALBUM };
static picker_opts_t view_opts;
static int  opens;
static bool visited, visited_before_open, suppress_overlay;

/* ---- screen ---- */

static lv_obj_t * scr, * band, * band_label, * field, * spacer, * namebar, * name_label, * msg;
static lv_timer_t * namebar_timer, * index_timer, * poll_timer;
static ui_grid_t  g;
static ui_list_t  mm, plm;
static int        nrows;                     /* artists */
static int *      line0;                     /* per artist: its first line */
static int *      nlines;                    /* per artist: lines (at least one) */
static int        total_lines, first_line;
static int        sel_row, sel_k;            /* the artist, and -1 = its tile, else the album */
static int        want_col;                  /* the column aimed for across Up/Down: 0 the tile, 1.. the album slots */
static bool       shown;
static unsigned   seed, art_gen;
static int        list_gen;

static void (*artist_cb)(int);
static void (*album_cb)(const library_t *, int, const char *, const tile_style_t *);
static void (*exit_cb)(void);
static void (*style_cb)(const char *);

/* ---- rows and tiles ---- */

typedef struct {
    int        artist;           /* -1 = free */
    int        k;                /* -1 = the artist tile, else the album's index in the artist */
    lv_obj_t * obj, * img, * t_box;
    int        shown_px;
} tile_t;

typedef struct {
    int        artist;           /* -1 = free */
    lv_obj_t * obj;              /* the artist's lines, nlines * g.cell tall */
    tile_style_t * styles;       /* 0 = the artist tile, 1.. = the albums */
    int        nstyles;
} rowobj_t;

static rowobj_t rowobjs[MAX_ROWOBJ];
static tile_t   tiles[MAX_TILES];

typedef struct { int artist, k, px; uint8_t * data; lv_image_dsc_t dsc; size_t bytes; unsigned stamp; } bmp_t;
static bmp_t *  bmps[BMP_MAX];
static int      nbmps;
static size_t   bmp_bytes;
static unsigned bmp_clock;

/* ------------------------------------------------------------ helpers */

static int cols(void) { return g.cols; }
static int rows_on(void) { return g.rows; }
static int slots(void) { return cols() - 1; }           /* album slots a line holds (measure() keeps cols >= 2) */
static bool ring_mode(void) { return rows_on() == 1; }

/* Where an album sits: its line (absolute) and its column (1..slots). */
/* An artist's albums share its lines evenly (Ian, 14 Oct: 7 albums in
 * rows of 5 as 4 + 3, 6 as 3 + 3): `base` a line, the first `extra` lines
 * one more. Line `li` (from the artist's first) starts at album
 * line_start() and holds line_len(). */
static const artist_t * artist_of(int r);
static int line_base(int a)  { int n = artist_of(a)->nalbums; return n > 0 ? n / nlines[a] : 0; }
static int line_extra(int a) { int n = artist_of(a)->nalbums; return n > 0 ? n % nlines[a] : 0; }
static int line_start(int a, int li) { int b = line_base(a), e = line_extra(a); return li < e ? li * (b + 1) : e * (b + 1) + (li - e) * b; }
static int line_len(int a, int li) { return li < line_extra(a) ? line_base(a) + 1 : line_base(a); }
static int line_in(int a, int k)
{
    if(k < 0) return 0;
    int b = line_base(a), e = line_extra(a);
    if(b <= 0) return 0;
    return k < e * (b + 1) ? k / (b + 1) : e + (k - e * (b + 1)) / b;
}
static int line_of(int a, int k) { return line0[a] + line_in(a, k); }
static int col_of(int a, int k) { return k < 0 ? 0 : k - line_start(a, line_in(a, k)) + 1; }

/* The albums on an artist's lines lo..hi (absolute), clipped to the artist. */
static void albums_on_lines(int a, int lo, int hi, int * klo, int * khi)
{
    int l0 = lo - line0[a], l1 = hi - line0[a];
    if(l0 < 0) l0 = 0;
    if(l1 > nlines[a] - 1) l1 = nlines[a] - 1;
    if(l1 < l0 || artist_of(a)->nalbums == 0) { *klo = 0; *khi = -1; return; }
    *klo = line_start(a, l0);
    *khi = line_start(a, l1) + line_len(a, l1) - 1;
}
static int artist_at_line(int line)
{
    for(int a = 0; a < nrows; a++) if(line < line0[a] + nlines[a]) return a;
    return nrows - 1;
}

static const artist_t * artist_of(int r) { return artists_get(r); }
static const lib_album_t * album_of(int r, int k)
{
    const artist_t * a = artist_of(r);
    if(!a || k < 0 || k >= a->nalbums) return NULL;
    return &libs_get(a->albums[k].src)->lib.albums[a->albums[k].album];
}
static const char * card_of(int r, int k) { const artist_t * a = artist_of(r); return libs_get(a->albums[k].src)->card; }

static bool tile_has_art(int r, int k)
{
    if(k < 0) return artist_of(r) && artist_of(r)->pic != NULL;
    const lib_album_t * al = album_of(r, k);
    return al && al->has_art;
}

/* ------------------------------------------------------------ options */

static void view_key(const char * what, char * out, size_t len) { snprintf(out, len, "%s-artists-%s", what, ui_screen_kind_name()); }

static void read_options(void)
{
    char key[64], v[24];
    view_opts.style_count = 0;
    view_opts.styles[view_opts.style_count++] = T(S_STYLE_ROWS);
    view_opts.styles[view_opts.style_count++] = T(S_STYLE_GRID);
    view_opts.styles[view_opts.style_count++] = T(S_STYLE_LIST);
    view_opts.style = 0;
    view_key("size", key, sizeof(key));
    config_read(key, v, sizeof(v), ui_size_name(UI_SIZE_DEFAULT));
    view_opts.size = UI_SIZE_DEFAULT;
    for(int i = 0; i < UI_SIZE_COUNT; i++) if(strcmp(ui_size_name((ui_size_t)i), v) == 0 && ui_size_offered((ui_size_t)i)) view_opts.size = (ui_size_t)i;
    /* "Opens as" in the sort row's place: what Select on an artist opens */
    view_opts.sort_name = T(S_PICKER_OPENS);
    view_opts.sort_count = 3;
    view_opts.sorts[OPENS_ALBUMS] = T(S_OPENS_ALBUMS);
    view_opts.sorts[OPENS_TRACKS] = T(S_OPENS_TRACKS);
    view_opts.sorts[OPENS_TRACKS_ALBUM] = T(S_OPENS_TRACKS_ALBUM);
    view_key("opens", key, sizeof(key));
    config_read(key, v, sizeof(v), "albums");
    opens = strcmp(v, "tracks") == 0 ? OPENS_TRACKS : strcmp(v, "tracks-album") == 0 ? OPENS_TRACKS_ALBUM : OPENS_ALBUMS;
    view_opts.sort = opens;
    view_key("names", key, sizeof(key));
    view_opts.names_row = true;
    view_opts.names = config_read_bool(key, true);
    view_key("jump", key, sizeof(key));
    view_opts.strip = config_read_bool(key, true);
    view_opts.jump_row = false;
    view_key("visited", key, sizeof(key));
    visited = config_read_bool(key, false);
}

static void write_options(void)
{
    char key[64];
    view_key("size", key, sizeof(key)); config_write(key, ui_size_name(view_opts.size));
    view_key("opens", key, sizeof(key)); config_write(key, opens == OPENS_TRACKS ? "tracks" : opens == OPENS_TRACKS_ALBUM ? "tracks-album" : "albums");
    view_key("names", key, sizeof(key)); config_write(key, view_opts.names ? "1" : "0");
    view_key("jump", key, sizeof(key)); config_write(key, view_opts.strip ? "1" : "0");
    view_key("style", key, sizeof(key)); config_write(key, view_opts.style == 1 ? "grid" : view_opts.style == 2 ? "list" : "rows");
}

static void mark_visited(void)
{
    if(visited) return;
    visited = true;
    char key[64];
    view_key("visited", key, sizeof(key));
    config_write(key, "1");
}

/* ------------------------------------------------------------ bitmaps */

static bmp_t * bmp_find(int artist, int k, int px)
{
    for(int i = 0; i < nbmps; i++) if(bmps[i]->artist == artist && bmps[i]->k == k && bmps[i]->px == px) { bmps[i]->stamp = ++bmp_clock; return bmps[i]; }
    return NULL;
}

static bool bmp_in_use(const bmp_t * b)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].artist >= 0 && tiles[i].img && lv_image_get_src(tiles[i].img) == &b->dsc) return true;
    return false;
}

static void bmp_drop(int i)
{
    bmp_t * b = bmps[i];
    for(int t = 0; t < MAX_TILES; t++)
        if(tiles[t].artist >= 0 && tiles[t].img && lv_image_get_src(tiles[t].img) == &b->dsc) {
            lv_image_set_src(tiles[t].img, NULL);
            lv_obj_add_flag(tiles[t].img, LV_OBJ_FLAG_HIDDEN);
            tiles[t].shown_px = 0;
        }
    lv_image_cache_drop(&b->dsc);
    free(b->data);
    bmp_bytes -= b->bytes;
    free(b);
    bmps[i] = bmps[nbmps - 1];
    nbmps--;
}

static void bmp_make_room(size_t need)
{
    while((nbmps >= BMP_MAX || bmp_bytes + need > BMP_BUDGET) && nbmps > 0) {
        int victim = -1;
        for(int i = 0; i < nbmps; i++) if(!bmp_in_use(bmps[i]) && (victim < 0 || bmps[i]->stamp < bmps[victim]->stamp)) victim = i;
        if(victim < 0) for(int i = 0; i < nbmps; i++) if(victim < 0 || bmps[i]->stamp < bmps[victim]->stamp) victim = i;
        bmp_drop(victim);
    }
}

static bmp_t * bmp_add(int artist, int k, int px, uint8_t * data, int w, int h)
{
    size_t bytes = (size_t)w * (size_t)h * 4;
    bmp_make_room(bytes);
    bmp_t * b = calloc(1, sizeof(*b));
    if(!b) { free(data); return NULL; }
    bmps[nbmps++] = b;
    b->artist = artist; b->k = k; b->px = px; b->data = data; b->bytes = bytes; b->stamp = ++bmp_clock;
    b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    b->dsc.header.w = (uint32_t)w; b->dsc.header.h = (uint32_t)h; b->dsc.header.stride = (uint32_t)w * 4;
    b->dsc.data_size = (uint32_t)bytes; b->dsc.data = data;
    bmp_bytes += bytes;
    return b;
}

static void bmp_clear_all(void) { list_gen++; while(nbmps) bmp_drop(0); }

/* the thumbs id of a tile: artist * 512 + (k + 1) */
static int tile_id(int artist, int k) { return ROWS_BASE + artist * 512 + (k + 1); }

/* ------------------------------------------------------------- tiles */

static tile_t * tile_of(int artist, int k)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].artist == artist && tiles[i].k == k) return &tiles[i];
    return NULL;
}

static rowobj_t * rowobj_of(int artist)
{
    for(int i = 0; i < MAX_ROWOBJ; i++) if(rowobjs[i].artist == artist) return &rowobjs[i];
    return NULL;
}

/* A tile's place inside its artist's row object: album k at (col, line)
 * cells; the artist tile at the left, spanning the artist's lines. */
static void tile_cell(const tile_t * t, int * x, int * y, int * h_cells)
{
    if(t->k < 0) { *x = 0; *y = 0; *h_cells = nlines[t->artist]; return; }
    *x = col_of(t->artist, t->k) * g.cell;
    *y = (line_of(t->artist, t->k) - line0[t->artist]) * g.cell;
    *h_cells = 1;
}

/* Place a tile of `side` px in its cell; a grown tile stays inside the
 * field's bounds. The artist tile is `side` wide and as tall as its lines. */
static void place_tile(tile_t * t, int side)
{
    int cx, cy, hc;
    tile_cell(t, &cx, &cy, &hc);
    int inset = (g.cell - side) / 2;
    int x = cx + inset, y = cy + inset;
    int h = hc == 1 ? side : hc * g.cell - (g.cell - side);
    if(side > g.cell) {
        int abs_y = line0[t->artist] * g.cell + y, top = first_line * g.cell;
        if(x < 0) x = 0;
        if(x + side > g.w) x = g.w - side;
        if(h <= g.h) {
            if(abs_y < top) y += top - abs_y;
            if(abs_y + h > top + g.h) y -= abs_y + h - (top + g.h);
        }
    }
    lv_obj_set_pos(t->obj, x, y);
    lv_obj_set_size(t->obj, side, h);
    if(t->img && t->k < 0) lv_obj_set_size(t->img, side, side);        /* the picture: the top square */
    if(t->t_box) {
        /* the name box: centred in the tile, scaled with it */
        lv_obj_set_pos(t->t_box, (side - g.tile) / 2, (h - g.tile) / 2);
        lv_obj_set_style_transform_scale(t->t_box, 256 * side / g.tile, 0);
    }
}

static void tile_apply_bitmap(tile_t * t, bmp_t * b)
{
    lv_image_set_src(t->img, &b->dsc);
    lv_obj_remove_flag(t->img, LV_OBJ_FLAG_HIDDEN);
    t->shown_px = b->px;
    if(t->t_box) lv_obj_add_flag(t->t_box, LV_OBJ_FLAG_HIDDEN);
}

static bool is_sel(const tile_t * t) { return t->artist == sel_row && t->k == sel_k; }

/* The artist tile grows only when it is one line tall; a tall one just
 * comes forward with a shadow. */
static bool tile_grows(const tile_t * t) { (void)t; return !ring_mode(); }

static void tile_refresh_bitmap(tile_t * t)
{
    if(!tile_has_art(t->artist, t->k)) return;
    int want = (is_sel(t) && tile_grows(t)) ? g.tile_focus : g.tile;
    bmp_t * b = bmp_find(t->artist, t->k, want);
    if(!b) b = bmp_find(t->artist, t->k, want == g.tile ? g.tile_focus : g.tile);
    if(b) { if(t->shown_px != b->px || lv_image_get_src(t->img) != &b->dsc) tile_apply_bitmap(t, b); }
}

static tile_t * tile_create(rowobj_t * ro, int k)
{
    tile_t * t = NULL;
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].artist < 0) { t = &tiles[i]; break; }
    if(!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->artist = ro->artist;
    t->k = k;
    const tile_style_t * st = (ro->styles && k + 1 < ro->nstyles) ? &ro->styles[k + 1] : NULL;
    uint32_t bg = 0x333333, ink = 0xEEEEEE;
    if(k >= 0 && album_of(ro->artist, k) && album_of(ro->artist, k)->has_art && album_of(ro->artist, k)->colour) { bg = album_of(ro->artist, k)->colour; ink = 0xFFFFFF; }
    else if(st) { bg = st->bg; ink = st->ink; }
    t->obj = lv_obj_create(ro->obj);
    lv_obj_remove_style_all(t->obj);
    lv_obj_set_style_bg_color(t->obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(t->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(t->obj, lv_color_hex(ink), 0);
    lv_obj_set_style_shadow_color(t->obj, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(t->obj, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(t->obj, 0, 0);
    lv_obj_set_style_outline_color(t->obj, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_set_style_outline_opa(t->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_pad(t->obj, 0, 0);
    lv_obj_set_style_outline_width(t->obj, 0, 0);
    lv_obj_remove_flag(t->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    t->img = lv_image_create(t->obj);
    if(k < 0) lv_obj_set_size(t->img, g.tile, g.tile); else lv_obj_set_size(t->img, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(t->img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(t->img, LV_OBJ_FLAG_HIDDEN);
    if(!tile_has_art(ro->artist, k)) {
        tiletext_t tt;
        const artist_t * a = artist_of(ro->artist);
        if(k < 0) tiletext_make(t->obj, g.tile, a->name, "", st, &tt);
        else tiletext_make(t->obj, g.tile, album_of(ro->artist, k)->title, "", st, &tt);
        t->t_box = tt.box;
    }
    else if(k < 0) {
        /* an artist's picture: the name over it, small, at the bottom */
        lv_obj_t * l = lv_label_create(t->obj);
        lv_obj_set_style_text_font(l, ui_font_px(g.tile / 11 < 14 ? 14 : g.tile / 11), 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_set_style_bg_color(l, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(l, g.tile / 30, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(l, artist_of(ro->artist)->name);
        lv_obj_set_width(l, g.tile);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, g.tile - lv_font_get_line_height(ui_font_px(g.tile / 11 < 14 ? 14 : g.tile / 11)) - 2 * (g.tile / 30));
    }
    place_tile(t, g.tile);
    tile_refresh_bitmap(t);
    return t;
}

static void tile_delete(tile_t * t)
{
    if(t->obj) lv_obj_delete(t->obj);
    memset(t, 0, sizeof(*t));
    t->artist = -1;
    t->k = -2;
}

/* ------------------------------------------------------------- focus */

static void zoom_exec(void * obj, int32_t v)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].obj == obj) { place_tile(&tiles[i], g.tile + (g.tile_focus - g.tile) * v / 100); return; }
}

static void focus_tile(tile_t * t, bool on, bool animate)
{
    if(!t) return;
    lv_anim_delete(t->obj, zoom_exec);
    if(ring_mode()) {
        int gap = g.cell - g.tile;
        int ring = cols() > 1 ? (gap / 2 < 3 ? 3 : gap / 2 > 8 ? 8 : gap / 2) : 0;
        lv_obj_set_style_shadow_width(t->obj, 0, 0);
        lv_obj_set_style_outline_width(t->obj, on ? ring : 0, 0);
        place_tile(t, g.tile);
        tile_refresh_bitmap(t);
        return;
    }
    lv_obj_set_style_outline_width(t->obj, 0, 0);
    lv_obj_set_style_shadow_width(t->obj, on ? g.tile / 6 : 0, 0);
    if(on) {
        lv_obj_move_foreground(t->obj);
        rowobj_t * ro = rowobj_of(t->artist);
        if(ro) lv_obj_move_foreground(ro->obj);
        if(animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, t->obj);
            lv_anim_set_exec_cb(&a, zoom_exec);
            lv_anim_set_values(&a, 0, 100);
            lv_anim_set_duration(&a, ZOOM_MS);
            lv_anim_start(&a);
        }
        else place_tile(t, g.tile_focus);
    }
    else place_tile(t, g.tile);
    tile_refresh_bitmap(t);
}

static tile_t * sel_tile(void) { return tile_of(sel_row, sel_k); }

/* ------------------------------------------------------------- rows */

static void want_update(void);

/* The window of lines whose tiles exist: the screen and one line either side. */
static int win_lo, win_hi;

/* The album tiles an artist's row needs: those on the window's lines. */
static void row_fill(rowobj_t * ro)
{
    const artist_t * a = artist_of(ro->artist);
    int r = ro->artist;
    for(int i = 0; i < MAX_TILES; i++) {
        if(tiles[i].artist != r || tiles[i].k < 0) continue;
        int line = line_of(r, tiles[i].k);
        if(line < win_lo || line > win_hi) tile_delete(&tiles[i]);
    }
    if(!tile_of(r, -1)) tile_create(ro, -1);
    int klo, khi;
    albums_on_lines(r, win_lo, win_hi, &klo, &khi);
    (void)a;
    for(int k = klo; k <= khi; k++) if(!tile_of(r, k)) tile_create(ro, k);
}

static void row_ext_cb(lv_event_t * e)
{
    int32_t * s = lv_event_get_param(e);
    int32_t need = (g.tile_focus - g.tile) / 2 + g.tile / 6 + 4;
    if(*s < need) *s = need;
}

static rowobj_t * row_create(int r)
{
    rowobj_t * ro = NULL;
    for(int i = 0; i < MAX_ROWOBJ; i++) if(rowobjs[i].artist < 0) { ro = &rowobjs[i]; break; }
    if(!ro) return NULL;
    memset(ro, 0, sizeof(*ro));
    ro->artist = r;
    const artist_t * a = artist_of(r);
    ro->obj = lv_obj_create(field);
    lv_obj_remove_style_all(ro->obj);
    lv_obj_remove_flag(ro->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* a grown cover and its shadow draw past the row's edge: LVGL clips a
     * child to its parent's area plus the parent's "extra draw size", so the
     * row claims that much (0.20d: OVERFLOW_VISIBLE alone still cut the art) */
    lv_obj_add_flag(ro->obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(ro->obj, row_ext_cb, LV_EVENT_REFR_EXT_DRAW_SIZE, NULL);
    lv_obj_refresh_ext_draw_size(ro->obj);
    lv_obj_set_pos(ro->obj, 0, line0[r] * g.cell);
    lv_obj_set_size(ro->obj, g.w, nlines[r] * g.cell);
    /* colours and faces for the artist tile and every album, dealt as the
     * grid deals: no two neighbours alike */
    ro->nstyles = 1 + a->nalbums;
    ro->styles = calloc((size_t)ro->nstyles, sizeof(tile_style_t));
    bool * noart = calloc((size_t)ro->nstyles, sizeof(bool));
    int * letters = calloc((size_t)ro->nstyles, sizeof(int));
    if(ro->styles && noart && letters) {
        noart[0] = a->pic == NULL;
        letters[0] = count_letters(a->name);
        for(int k = 0; k < a->nalbums; k++) { noart[k + 1] = !album_of(r, k)->has_art; letters[k + 1] = count_letters(album_of(r, k)->title); }
        tilestyle_deal(seed ^ ((unsigned)r * 2654435761u), ro->nstyles, slots() + 1, noart, letters, ro->styles);
    }
    free(noart);
    free(letters);
    row_fill(ro);
    return ro;
}

static void row_delete(rowobj_t * ro)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].artist == ro->artist) tile_delete(&tiles[i]);
    if(ro->obj) lv_obj_delete(ro->obj);
    free(ro->styles);
    memset(ro, 0, sizeof(*ro));
    ro->artist = -1;
}

static void rows_clear(void)
{
    for(int i = 0; i < MAX_ROWOBJ; i++) if(rowobjs[i].artist >= 0) row_delete(&rowobjs[i]);
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].artist >= 0) tile_delete(&tiles[i]);
}

/* Keep the artists whose lines touch a..b, with the tiles on those lines. */
static void window_set(int a, int b)
{
    if(a < 0) a = 0;
    if(b > total_lines - 1) b = total_lines - 1;
    win_lo = a; win_hi = b;
    for(int i = 0; i < MAX_ROWOBJ; i++) {
        if(rowobjs[i].artist < 0) continue;
        int r = rowobjs[i].artist;
        if(line0[r] + nlines[r] - 1 < a || line0[r] > b) row_delete(&rowobjs[i]);
    }
    for(int r = nrows ? artist_at_line(a) : 0; r < nrows && line0[r] <= b; r++) {
        rowobj_t * ro = rowobj_of(r);
        if(!ro) row_create(r); else row_fill(ro);
    }
    tile_t * ts = sel_tile();
    if(ts) focus_tile(ts, picker_mode() != PICKER_STRIP, false);
}

static void window_normal(void)
{
    window_set(first_line - 1, first_line + rows_on());
    want_update();
}

/* The decoder's wanted list: the tiles on screen with art, the selected
 * one's big size first, then the line either side. */
static void want_update(void)
{
    static thumb_req_t reqs[THUMBS_MAX_WANT];
    int n = 0;
    if(nrows == 0) { thumbs_want(reqs, 0); return; }
    for(int pass = 0; pass < 2 && n < THUMBS_MAX_WANT; pass++) {
        int lo = first_line - (pass ? 1 : 0), hi = first_line + rows_on() - 1 + (pass ? 1 : 0);
        for(int r = artist_at_line(lo < 0 ? 0 : lo); r < nrows && line0[r] <= hi && n < THUMBS_MAX_WANT; r++) {
            const artist_t * a = artist_of(r);
            int klo, khi;
            albums_on_lines(r, lo, hi, &klo, &khi);
            for(int k = -1; k <= khi && n < THUMBS_MAX_WANT; k++) {
                if(k >= 0 && k < klo) continue;
                if(!tile_has_art(r, k)) continue;
                bool this_sel = r == sel_row && k == sel_k;
                int px = (this_sel && !ring_mode()) ? g.tile_focus : g.tile;
                if(bmp_find(r, k, px)) continue;
                bool dup = false;
                for(int i = 0; i < n; i++) if(reqs[i].id == tile_id(r, k) && reqs[i].px == px) { dup = true; break; }
                if(dup) continue;
                if(k < 0) snprintf(reqs[n].path, sizeof(reqs[n].path), "%s", a->pic);
                else {
                    const lib_album_t * al = album_of(r, k);
                    int si = 0;
                    while(si < LIB_THUMB_COUNT - 1 && lib_thumb_px[si] < px) si++;
                    while(si > 0 && !(al->thumb_sizes & (1 << si))) si--;
                    cache_thumb_path(card_of(r, k), al, si, reqs[n].path, sizeof(reqs[n].path));
                }
                reqs[n].id = tile_id(r, k);
                reqs[n].gen = list_gen;
                reqs[n].px = px;
                n++;
            }
        }
    }
    thumbs_want(reqs, n);
}

static void poll_cb(lv_timer_t * tm)
{
    (void)tm;
    thumb_res_t r;
    int budget = 6;
    while(budget-- > 0 && thumbs_poll(&r)) {
        if(!r.pixels) continue;
        int id = r.id - ROWS_BASE;
        if(r.gen != list_gen || id < 0) { free(r.pixels); continue; }
        int artist = id / 512, k = id % 512 - 1;
        if(artist >= nrows) { free(r.pixels); continue; }
        if(bmp_find(artist, k, r.px)) { free(r.pixels); continue; }
        bmp_add(artist, k, r.px, r.pixels, r.w, r.h);
        tile_t * t = tile_of(artist, k);
        if(t) tile_refresh_bitmap(t);
    }
}

/* ------------------------------------------------------------ names */

static void namebar_hide_cb(lv_timer_t * tm)
{
    (void)tm;
    lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    power_indicator_yield(false);
    namebar_timer = NULL;
}

static void show_name(void)
{
    if(nrows == 0) return;
    const artist_t * a = artist_of(sel_row);
    char text[600];
    if(sel_k < 0) {
        char count[64];
        artists_count_text(sel_row, opens != OPENS_ALBUMS, count, sizeof(count));
        snprintf(text, sizeof(text), "%s \xC2\xB7 %s", a->name, count);
    }
    else {
        const lib_album_t * al = album_of(sel_row, sel_k);
        if(al && al->year) snprintf(text, sizeof(text), "%s \xE2\x80\x93 %s \xC2\xB7 %d", a->name, al->title, al->year);
        else snprintf(text, sizeof(text), "%s \xE2\x80\x93 %s", a->name, al ? al->title : "");
    }
    if(!view_opts.names) {
        if(g.band_h) lv_label_set_text(band_label, "");
        else lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if(g.band_h) { lv_label_set_text(band_label, text); return; }
    if(picker_mode() == PICKER_STRIP) return;
    lv_label_set_text(name_label, text);
    lv_obj_remove_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(namebar);
    power_indicator_yield(true);
    if(namebar_timer) lv_timer_reset(namebar_timer);
    else { namebar_timer = lv_timer_create(namebar_hide_cb, NAMEBAR_MS, NULL); lv_timer_set_repeat_count(namebar_timer, 1); }
}

/* ------------------------------------------------------------ moving */

static void scroll_exec(void * obj, int32_t v) { lv_obj_scroll_to_y(obj, v, LV_ANIM_OFF); }
static void scroll_done_cb(lv_anim_t * a) { (void)a; window_normal(); }

/* Scroll so `line` is the top line on screen. */
static void scroll_to_line(int line, int ms)
{
    if(line > total_lines - rows_on()) line = total_lines - rows_on();
    if(line < 0) line = 0;
    int from = first_line;
    first_line = line;
    int far = from > line ? from - line : line - from;
    if(far > rows_on()) ms = 0;                   /* a jump, not a slide */
    int lo = (from < line ? from : line) - 1, hi = (from > line ? from : line) + rows_on();
    if(ms > 0) window_set(lo, hi);
    lv_anim_delete(field, scroll_exec);
    if(ms <= 0 || from == line) { lv_obj_scroll_to_y(field, line * g.cell, LV_ANIM_OFF); window_normal(); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, field);
    lv_anim_set_exec_cb(&a, scroll_exec);
    lv_anim_set_values(&a, lv_obj_get_scroll_y(field), line * g.cell);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, scroll_done_cb);
    lv_anim_start(&a);
}

/* Select artist `row`'s tile (k < 0) or album k, and bring its line on
 * screen; `page` slides a screenful's worth. */
static void set_sel(int row, int k, bool page)
{
    if(nrows == 0) return;
    if(row < 0) row = 0;
    if(row > nrows - 1) row = nrows - 1;
    const artist_t * a = artist_of(row);
    if(k > a->nalbums - 1) k = a->nalbums - 1;
    if(k < 0) k = -1;
    tile_t * old = sel_tile();
    bool same = row == sel_row && k == sel_k;
    sel_row = row;
    sel_k = k;
    if(old) focus_tile(old, false, false);
    /* the lines to show: the album's, or all of an artist's for its tile
     * (0.20b: a tall tile came on half shown, then the next press jumped two) */
    int line = line_of(row, k), last = k < 0 ? line0[row] + nlines[row] - 1 : line;
    if(last - line + 1 > rows_on()) last = line + rows_on() - 1;
    int top = first_line, bottom = first_line + rows_on() - 1;
    bool pill = picker_mode() == PICKER_STRIP;
    if(line < top || last > bottom || pill) {
        int target = line < top ? line : last > bottom ? last - rows_on() + 1 : first_line;
        if(pill && rows_on() > 1) target = line - 1;
        if(target < 0) target = 0;
        if(target != first_line) scroll_to_line(target, page ? PAGE_MS : LINE_MS);
    }
    tile_t * tn = sel_tile();
    if(tn) focus_tile(tn, !pill, !pill && !same);
    want_update();
    show_name();
}

/* The album of artist `a` on `line` in column `col` (1..slots), the line's
 * last when it is shorter; -1 (the tile) when there is none. */
static int album_at(int a, int line, int col)
{
    const artist_t * ar = artist_of(a);
    if(col <= 0 || ar->nalbums == 0) return -1;
    int li = line - line0[a];
    if(li < 0) li = 0;
    if(li > nlines[a] - 1) li = nlines[a] - 1;
    int len = line_len(a, li);
    if(col > len) col = len;
    return line_start(a, li) + col - 1;
}

/* Up/Down: the line above or below (the list wraps), in the column aimed
 * for; from an artist's tile to the next artist's tile. */
static void move_line(int dir)
{
    if(sel_k < 0) {
        int row = sel_row + dir;
        if(row < 0) row = nrows - 1;
        if(row > nrows - 1) row = 0;
        set_sel(row, -1, false);
        return;
    }
    int line = line_of(sel_row, sel_k) + dir;
    if(line < 0) line = total_lines - 1;
    if(line > total_lines - 1) line = 0;
    int a = artist_at_line(line);
    set_sel(a, album_at(a, line, want_col), false);
}

static void move_right(void)
{
    const artist_t * a = artist_of(sel_row);
    if(sel_k < 0) { if(a->nalbums > 0) { set_sel(sel_row, 0, false); want_col = 1; } return; }
    if(sel_k < a->nalbums - 1) { set_sel(sel_row, sel_k + 1, false); want_col = col_of(sel_row, sel_k); return; }
    /* the artist's last album: a page down, the column kept */
    if(total_lines <= rows_on()) return;
    int line = line_of(sel_row, sel_k) + rows_on();
    if(line > total_lines - 1) line = total_lines - 1;
    int b = artist_at_line(line);
    scroll_to_line(first_line + rows_on(), PAGE_MS);
    int col = col_of(sel_row, sel_k);
    set_sel(b, album_at(b, line, col), true);
    want_col = col;
}

static void move_left(void)
{
    if(sel_k < 0) {
        /* the artist tile: a page up */
        if(total_lines <= rows_on()) return;
        int line = line0[sel_row] - rows_on();
        if(line < 0) line = 0;
        scroll_to_line(first_line - rows_on(), PAGE_MS);
        set_sel(artist_at_line(line), -1, true);
        want_col = 0;
        return;
    }
    if(col_of(sel_row, sel_k) == 1) { set_sel(sel_row, -1, false); want_col = 0; return; }
    set_sel(sel_row, sel_k - 1, false);
    want_col = col_of(sel_row, sel_k);
}

/* ------------------------------------------------------------ layout */

static void measure(void)
{
    /* the grid's shape at the picker's size, with at least one album slot
     * beside the artist tile (the handheld's XXL is one cover across) */
    ui_size_t s = view_opts.size;
    ui_grid_metrics(s, &g);
    while(g.cols < 2 && s + 1 < UI_SIZE_COUNT) { s++; ui_grid_metrics(s, &g); }
    ui_list_metrics(UI_SIZE_DEFAULT, &mm);
    plm = mm;
    plm.x = g.x; plm.y = g.y; plm.w = g.w; plm.h = g.h;
}

/* Each artist's lines at this width: as many as their albums need. */
static void layout_lines(void)
{
    total_lines = 0;
    for(int r = 0; r < nrows; r++) {
        int n = artist_of(r)->nalbums;
        nlines[r] = n <= 0 ? 1 : (n + slots() - 1) / slots();
        line0[r] = total_lines;
        total_lines += nlines[r];
    }
}

static void layout_screen(void)
{
    lv_obj_set_pos(field, g.x, g.y);
    lv_obj_set_size(field, g.w, g.h);
    lv_obj_set_size(spacer, 1, total_lines * g.cell);
    lv_obj_set_pos(spacer, 0, 0);
    if(g.band_h) {
        lv_obj_remove_flag(band, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(band, g.x + mm.margin, g.band_y);
        lv_obj_set_size(band, g.w - 2 * mm.margin, g.band_h);
        lv_obj_set_style_text_font(band_label, ui_font_px(mm.font_px), 0);
        lv_obj_align(band_label, LV_ALIGN_LEFT_MID, 0, 0);
    }
    else lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(namebar, g.x, g.y + mm.margin);
    lv_obj_set_size(namebar, g.w, mm.row_h);
    lv_obj_set_style_text_font(name_label, ui_font_px(mm.font_px), 0);
    lv_obj_set_style_text_font(msg, ui_font_px(mm.font_px), 0);
    lv_obj_center(msg);
}

static void build_labels(void)
{
    const char ** names = malloc(sizeof(char *) * (size_t)(nrows > 0 ? nrows : 1));
    if(!names) return;
    int n = 0;
    for(int r = 0; r < nrows; r++) if(!artist_of(r)->various) names[n++] = artist_of(r)->name;
    picker_labels_from_names(&view_opts, names, n, false);
    free(names);
    view_opts.jump = view_opts.label_count ? 0 : -1;
}

static int row_label(int r)
{
    if(artist_of(r)->various) return -1;
    return picker_label_of(&view_opts, artist_of(r)->name);
}

static void rebuild(void)
{
    rows_clear();
    bmp_clear_all();
    measure();
    layout_lines();
    layout_screen();
    picker_relayout(&plm);
    if(sel_row > nrows - 1) sel_row = nrows ? nrows - 1 : 0;
    if(nrows && sel_k > artist_of(sel_row)->nalbums - 1) sel_k = artist_of(sel_row)->nalbums - 1;
    int line = nrows ? line_of(sel_row, sel_k) : 0;
    first_line = line - rows_on() / 2;
    if(first_line > total_lines - rows_on()) first_line = total_lines - rows_on();
    if(first_line < 0) first_line = 0;
    lv_obj_scroll_to_y(field, first_line * g.cell, LV_ANIM_OFF);
    if(nrows == 0) {
        lv_label_set_text(msg, T(libs_loaded() ? S_ARTISTS_EMPTY : S_BROWSER_STARTING));
        lv_obj_remove_flag(msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
        if(g.band_h) lv_label_set_text(band_label, "");
        return;
    }
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    window_normal();
    if(g.band_h) show_name();
}

static void load(void)
{
    artists_refresh();
    art_gen = artists_generation();
    int n = artists_count();
    free(line0); free(nlines);
    line0 = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    nlines = calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    nrows = n;
    if(sel_row > nrows - 1) sel_row = nrows ? nrows - 1 : 0;
    if(sel_row < 0) sel_row = 0;
    sel_k = -1;
    want_col = 0;
    build_labels();
    list_gen++;
}

/* ------------------------------------------------------------ picker */

static bool is_long(void) { return total_lines > STRIP_MIN_ROWS * rows_on() && view_opts.label_count >= 3; }

static void on_jump(int label, void * ctx)
{
    (void)ctx;
    for(int r = 0; r < nrows; r++) if(row_label(r) == label) { set_sel(r, -1, true); want_col = 0; return; }
}

static void on_change(const picker_opts_t * o, void * ctx)
{
    (void)ctx;
    bool resize = o->size != view_opts.size, restyle = o->style != view_opts.style, renames = o->names != view_opts.names || o->sort != opens;
    view_opts.size = o->size;
    view_opts.strip = o->strip;
    view_opts.style = o->style;
    view_opts.names = o->names;
    view_opts.sort = o->sort;
    opens = o->sort;
    write_options();
    if(restyle && view_opts.style != 0) { if(style_cb) style_cb(view_opts.style == 1 ? "grid" : "list"); return; }
    if(resize) rebuild();
    if(renames) show_name();
}

static void open_strip(void);

static void on_close(bool back, void * ctx)
{
    picker_mode_t was = (picker_mode_t)(intptr_t)ctx;
    mark_visited();
    if(back && picker_mode() == PICKER_NONE && was == PICKER_STRIP) { if(exit_cb) exit_cb(); return; }
    if(was == PICKER_FULL && view_opts.strip && is_long() && !visited_before_open) open_strip();
    else { tile_t * t = sel_tile(); if(t) focus_tile(t, true, false); }
}

static void open_strip(void)
{
    if(nrows == 0) return;
    tile_t * t = sel_tile();
    if(t) focus_tile(t, false, false);
    int l = row_label(sel_row);
    view_opts.jump = l >= 0 ? l : 0;
    visited_before_open = visited;
    picker_open(PICKER_STRIP, &view_opts, on_jump, on_change, on_close, (void *)(intptr_t)PICKER_STRIP);
}

static void open_full(void)
{
    int l = nrows ? row_label(sel_row) : -1;
    view_opts.jump = l >= 0 ? l : 0;
    visited_before_open = visited;
    picker_open(PICKER_FULL, &view_opts, on_jump, on_change, on_close, (void *)(intptr_t)PICKER_FULL);
}

static void after_enter(void)
{
    if(suppress_overlay) { suppress_overlay = false; return; }
    if(nrows == 0) return;
    if(!visited) { open_full(); return; }
    if(view_opts.strip && is_long()) open_strip();
}

/* ------------------------------------------------------------ public */

static void index_timer_cb(lv_timer_t * tm)
{
    (void)tm;
    if(!shown) return;
    if(libs_refresh() || art_gen != artists_generation() || artists_refresh()) {
        const artist_t * a = artist_of(sel_row);
        char * keep = a ? strdup(a->name) : NULL;
        load();
        if(keep) { int i = artists_find(keep); if(i >= 0) sel_row = i; free(keep); }
        rebuild();
    }
}

void rows_create(void (*on_artist)(int), void (*on_album)(const library_t *, int, const char *, const tile_style_t *), void (*on_exit)(void), void (*on_style)(const char *))
{
    artist_cb = on_artist;
    album_cb = on_album;
    exit_cb = on_exit;
    style_cb = on_style;
    for(int i = 0; i < MAX_TILES; i++) { tiles[i].artist = -1; tiles[i].k = -2; }
    for(int i = 0; i < MAX_ROWOBJ; i++) rowobjs[i].artist = -1;
    thumbs_init();
    char theme_path[512];
    config_theme_file(theme_path, sizeof(theme_path));
    tilestyle_load(theme_path);
    read_options();
    measure();

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    band = lv_obj_create(scr);
    lv_obj_remove_style_all(band);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    band_label = lv_label_create(band);
    lv_label_set_long_mode(band_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(band_label, lv_color_hex(UI_COLOR_FG), 0);
    lv_obj_set_width(band_label, LV_PCT(100));
    lv_label_set_text(band_label, "");
    field = lv_obj_create(scr);
    lv_obj_remove_style_all(field);
    lv_obj_set_scroll_dir(field, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN);
    spacer = lv_obj_create(field);
    lv_obj_remove_style_all(spacer);
    namebar = lv_obj_create(scr);
    lv_obj_remove_style_all(namebar);
    lv_obj_remove_flag(namebar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    name_label = lv_label_create(namebar);
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(name_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(name_label, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(name_label, 14, 0);
    lv_obj_set_style_pad_ver(name_label, 6, 0);
    lv_obj_set_style_radius(name_label, 8, 0);
    lv_obj_set_width(name_label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(name_label, LV_PCT(96), 0);
    lv_obj_align(name_label, LV_ALIGN_CENTER, 0, 0);
    msg = lv_label_create(scr);
    lv_obj_set_style_text_color(msg, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    layout_screen();
    poll_timer = lv_timer_create(poll_cb, 40, NULL);
    index_timer = lv_timer_create(index_timer_cb, 3000, NULL);
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
}

void rows_show(bool fresh)
{
    shown = true;
    if(fresh) seed = (unsigned)time(NULL) ^ (unsigned)lv_tick_get() ^ (unsigned)rand();
    suppress_overlay = !fresh;
    read_options();
    lv_screen_load(scr);
    picker_attach(scr, &plm);
    lv_timer_resume(poll_timer);
    lv_timer_resume(index_timer);
    bool fresh_list = artists_refresh() || art_gen != artists_generation() || !line0;
    if(fresh_list) { int keep = sel_row; load(); if(keep < nrows) sel_row = keep; }
    rebuild();
    after_enter();
}

void rows_hide(void)
{
    shown = false;
    if(namebar_timer) { lv_timer_delete(namebar_timer); namebar_timer = NULL; }
    lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    power_indicator_yield(false);
    if(picker_mode() != PICKER_NONE) picker_close();
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
    thumbs_want(NULL, 0);
    rows_clear();
    bmp_clear_all();
}

bool rows_overlay_active(void) { return shown && picker_mode() != PICKER_NONE; }

void rows_toggle_picker(void)
{
    if(!shown) return;
    if(picker_mode() != PICKER_NONE) { picker_toggle_panel(); return; }
    open_full();
}

void rows_show_picker_style(void)
{
    if(!shown || nrows == 0) return;
    if(picker_mode() != PICKER_FULL) { if(picker_mode() != PICKER_NONE) picker_close(); open_full(); }
    picker_focus_style();
}

void rows_reload_options(void)
{
    ui_size_t old_size = view_opts.size;
    read_options();
    if(!shown) return;
    if(view_opts.size != old_size) rebuild();
    if(!view_opts.strip && picker_mode() == PICKER_STRIP) picker_close();
}

void rows_library_changed(void) { if(shown) lv_timer_ready(index_timer); }

bool rows_focus_artist(int artist)
{
    if(artist < 0 || artist >= nrows) return false;
    if(shown) { set_sel(artist, -1, false); want_col = 0; }
    else { sel_row = artist; sel_k = -1; want_col = 0; }
    return true;
}

int rows_selected_artist(void) { return nrows ? sel_row : -1; }

void rows_handle_action(ui_action_t a)
{
    if(picker_mode() != PICKER_NONE) { picker_handle_action(a); return; }
    if(nrows == 0) { if(a == ACT_BACK && exit_cb) exit_cb(); return; }
    switch(a) {
        case ACT_UP:    move_line(-1); break;
        case ACT_DOWN:  move_line(+1); break;
        case ACT_LEFT:  move_left(); break;
        case ACT_RIGHT: move_right(); break;
        case ACT_SELECT:
            if(sel_k < 0) { if(artist_cb) artist_cb(sel_row); }
            else {
                int k = sel_k;
                const lib_album_t * al = album_of(sel_row, k);
                rowobj_t * ro = rowobj_of(sel_row);
                if(al && album_cb) album_cb(&libs_get(artist_of(sel_row)->albums[k].src)->lib, artist_of(sel_row)->albums[k].album, card_of(sel_row, k),
                                            (!al->has_art && ro && k + 1 < ro->nstyles) ? &ro->styles[k + 1] : NULL);
            }
            break;
        case ACT_BACK:
            if(view_opts.strip && is_long()) open_strip();
            else if(exit_cb) exit_cb();
            break;
        default: break;
    }
}
