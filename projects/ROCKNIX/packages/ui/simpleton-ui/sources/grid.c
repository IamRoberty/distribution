/*
 * SimpletonOS UI - the album grid (implementation). See grid.h.
 *
 * Shape: a screen of its own holding
 *   band     (TV only) the info line at the top, "Artist - Album"
 *   field    the grid area, scrolling vertically over `rows_total` rows of
 *            `g.cell` each; tiles are children at absolute positions, only
 *            the rows on screen and one either side exist as objects
 *   namebar  (handheld) the momentary "Artist - Album" over the bottom row
 *   msg      "Starting library..." / "No albums yet" when there is nothing
 * and the shared picker attached over the field.
 *
 * Albums come from the library index of every card MPD has mounted
 * (card-<serial>/ at the root), merged and sorted here with the library's
 * own comparison; singles lying in collection folders are left out, as
 * Note 05 section 8 asks.
 *
 * Bitmaps: a small cache of decoded thumbnails keyed by (entry, px) with a
 * byte budget; the decoder (thumbs.c) is handed the wanted list - the
 * tiles on screen, then the page ahead - every time the view moves.
 */
#include "grid.h"
#include "cache.h"
#include "config.h"
#include "library.h"
#include "mpdc.h"
#include "picker.h"
#include "placeholder.h"
#include "strings.h"
#include "theme.h"
#include "thumbs.h"
#include "tilestyle.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop, not in lvgl.h */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_SOURCES   4
#define MAX_TILES     160                     /* (rows + 2) * cols, twice, on the biggest grid */
#define BMP_MAX       256
#define BMP_BUDGET    (32u * 1024u * 1024u)   /* bytes of decoded thumbnails kept */
#define NAMEBAR_MS    1400
#define ZOOM_MS       100
#define ROW_SLIDE_MS  110
#define PAGE_SLIDE_MS 160
#define STRIP_SCREENS 3                       /* the pill past this many screenfuls... */
#define STRIP_MIN_LABELS 3                    /* ...with at least this many letters   */
#define BIG_TILE_PX   260                     /* no-art tiles this big show the artist too */

/* ---- the library ---- */

typedef struct {
    library_t lib;
    char      card[64];
    char      idx_path[320];
    time_t    idx_mtime;
} source_t;

typedef struct { int src, album; int label; } entry_t;   /* label: index into view_opts.labels, -1 none */

static source_t sources[MAX_SOURCES];
static int      nsources;
static entry_t * entries;
static int       nentries;
static bool      lib_loaded;

/* ---- options ---- */

static picker_opts_t view_opts;
static lib_sort_t    sort_kind;
static bool          visited, visited_before_open;
static bool          suppress_overlay;

/* ---- screen ---- */

static lv_obj_t * scr, * band, * field, * spacer, * namebar, * msg;
/* A name label: the band's are plain; the handheld's are subtitle-style,
 * white with a black outline (eight black copies a step out behind the
 * coloured one — LVGL has no text stroke for bitmap fonts). */
typedef struct { lv_obj_t * box; lv_obj_t * l[9]; int n; } nlabel_t;
static nlabel_t band_main, band_tail;              /* the TV band's two labels: name, bracketed tail */
static nlabel_t name_main, name_tail;              /* the handheld subtitle's                         */
static lv_timer_t * namebar_timer, * index_timer;
static ui_grid_t  g;
static ui_list_t  mm;                 /* menu measurements: the bar, the band text */
static ui_list_t  plm;                /* the picker's idea of our area              */
static int        sel;                /* selected entry                             */
static int        first_row;          /* top row on screen                          */
static int        rows_total;
static bool       shown;
static unsigned   visit_seed;         /* no-art colourways: random per visit        */

static void (*open_cb)(const char *, const char *);
static void (*exit_cb)(void);

/* ---- tiles ---- */

typedef struct {
    int        entry;                 /* -1 = free                                  */
    lv_obj_t * obj;                   /* the tile: bg colour, shadow when focused   */
    lv_obj_t * img;                   /* the cover, hidden until a bitmap arrives   */
    lv_obj_t * t_box;                 /* no-art text box, NULL when the album has art; scaled with the tile */
    lv_obj_t * t_title, * t_artist, * t_extra;   /* its lines (t_extra: the bracketed tail) */
    int        shown_px;              /* px of the bitmap on the image, 0 = none    */
} tile_t;

static tile_t tiles[MAX_TILES];

/* ---- bitmaps ---- */

typedef struct {
    int            entry, px;
    uint8_t *      data;
    lv_image_dsc_t dsc;
    size_t         bytes;
    unsigned       stamp;
} bmp_t;

/* each record is its own allocation: a tile's image holds a pointer to the
 * record's descriptor, so the record must never move while it is shown */
static bmp_t *  bmps[BMP_MAX];
static int      nbmps;
static size_t   bmp_bytes;
static unsigned bmp_clock;
static int      list_gen;      /* bumped whenever the entries are re-sorted or reloaded */
static lv_timer_t * poll_timer;

/* ------------------------------------------------------------ helpers */

static const lib_album_t * album_of(int e)
{
    return &sources[entries[e].src].lib.albums[entries[e].album];
}

static int cols(void) { return g.cols; }
static int rows_on_screen(void) { return g.rows; }
static int row_of(int e) { return e / cols(); }
static int col_of(int e) { return e % cols(); }

static void view_key(const char * what, char * out, size_t len)
{
    snprintf(out, len, "%s-albums-%s", what, ui_screen_kind_name());
}

static ui_size_t read_size(void)
{
    char key[64], v[16];
    view_key("size", key, sizeof(key));
    config_read(key, v, sizeof(v), ui_size_name(UI_SIZE_DEFAULT));
    for(int i = 0; i < UI_SIZE_COUNT; i++)
        if(strcmp(ui_size_name((ui_size_t)i), v) == 0 && ui_size_offered((ui_size_t)i)) return (ui_size_t)i;
    return UI_SIZE_DEFAULT;
}

static lib_sort_t read_sort(void)
{
    char key[64], v[24];
    view_key("sort", key, sizeof(key));
    config_read(key, v, sizeof(v), library_sort_name(LIB_SORT_TITLE));
    for(int i = 0; i < LIB_SORT_COUNT; i++)
        if(strcmp(library_sort_name((lib_sort_t)i), v) == 0) return (lib_sort_t)i;
    return LIB_SORT_TITLE;
}

static void read_options(void)
{
    char key[64];
    view_opts.style = 0;
    view_opts.style_count = 1;
    view_opts.styles[0] = T(S_STYLE_GRID);
    view_opts.size = read_size();
    sort_kind = read_sort();
    view_opts.sort = (int)sort_kind;
    view_opts.sort_count = LIB_SORT_COUNT;
    view_opts.sorts[LIB_SORT_TITLE] = T(S_SORT_TITLE);
    view_opts.sorts[LIB_SORT_ARTIST_TITLE] = T(S_SORT_ARTIST_TITLE);
    view_opts.sorts[LIB_SORT_ARTIST_YEAR] = T(S_SORT_ARTIST_YEAR);
    view_opts.sorts[LIB_SORT_YEAR] = T(S_SORT_YEAR);
    view_opts.sorts[LIB_SORT_NEWEST] = T(S_SORT_NEWEST);
    view_key("jump", key, sizeof(key));
    view_opts.strip = config_read_bool(key, true);
    view_key("visited", key, sizeof(key));
    visited = config_read_bool(key, false);
}

static void write_options(void)
{
    char key[64];
    view_key("size", key, sizeof(key));
    config_write(key, ui_size_name(view_opts.size));
    view_key("sort", key, sizeof(key));
    config_write(key, library_sort_name(sort_kind));
    view_key("jump", key, sizeof(key));
    config_write(key, view_opts.strip ? "1" : "0");
}

static void mark_visited(void)
{
    if(visited) return;
    visited = true;
    char key[64];
    view_key("visited", key, sizeof(key));
    config_write(key, "1");
}

static const char * artist_text(const lib_album_t * a)
{
    if(a->various) return T(S_VARIOUS_ARTISTS);
    return a->artist;
}

/* ------------------------------------------------------------ bitmaps */

static bmp_t * bmp_find(int entry, int px)
{
    for(int i = 0; i < nbmps; i++) if(bmps[i]->entry == entry && bmps[i]->px == px) { bmps[i]->stamp = ++bmp_clock; return bmps[i]; }
    return NULL;
}

static bool bmp_in_use(const bmp_t * b)
{
    for(int i = 0; i < MAX_TILES; i++)
        if(tiles[i].entry >= 0 && tiles[i].img && lv_image_get_src(tiles[i].img) == &b->dsc) return true;
    return false;
}

static void bmp_drop(int i)
{
    bmp_t * b = bmps[i];
    /* a tile still showing it goes back to its colour */
    for(int t = 0; t < MAX_TILES; t++)
        if(tiles[t].entry >= 0 && tiles[t].img && lv_image_get_src(tiles[t].img) == &b->dsc) {
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
        /* the oldest that no tile is showing; else the oldest */
        int victim = -1;
        for(int i = 0; i < nbmps; i++)
            if(!bmp_in_use(bmps[i]) && (victim < 0 || bmps[i]->stamp < bmps[victim]->stamp)) victim = i;
        if(victim < 0) for(int i = 0; i < nbmps; i++) if(victim < 0 || bmps[i]->stamp < bmps[victim]->stamp) victim = i;
        bmp_drop(victim);
    }
}

static bmp_t * bmp_add(int entry, int px, uint8_t * data, int w, int h)
{
    size_t bytes = (size_t)w * (size_t)h * 4;
    bmp_make_room(bytes);
    bmp_t * b = calloc(1, sizeof(*b));
    if(!b) { free(data); return NULL; }
    bmps[nbmps++] = b;
    b->entry = entry;
    b->px = px;
    b->data = data;
    b->bytes = bytes;
    b->stamp = ++bmp_clock;
    b->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    b->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
    b->dsc.header.w = (uint32_t)w;
    b->dsc.header.h = (uint32_t)h;
    b->dsc.header.stride = (uint32_t)w * 4;
    b->dsc.data_size = (uint32_t)bytes;
    b->dsc.data = data;
    bmp_bytes += bytes;
    return b;
}

static void bmp_clear_all(void)
{
    list_gen++;
    while(nbmps) bmp_drop(0);
}

/* ------------------------------------------------------------- tiles */

static tile_t * tile_of(int entry)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].entry == entry) return &tiles[i];
    return NULL;
}

/* Place a tile of `side` px in its cell; a tile bigger than its cell (the
 * one that has come forward) is kept inside the field, so the edge columns
 * and rows are never cut off. */
static void place_tile(tile_t * t, int side)
{
    int row = row_of(t->entry), col = col_of(t->entry);
    int inset = (g.cell - side) / 2;
    int x = col * g.cell + inset, y = row * g.cell + inset;
    if(side > g.cell) {
        int top = first_row * g.cell;
        if(x < 0) x = 0;
        if(x + side > g.w) x = g.w - side;
        if(y < top) y = top;
        if(y + side > top + g.h) y = top + g.h - side;
    }
    lv_obj_set_pos(t->obj, x, y);
    lv_obj_set_size(t->obj, side, side);
    if(t->t_box) {
        /* the text grows with the tile, like a cover does: the box keeps the
         * resting size and is drawn scaled about its centre */
        lv_obj_set_pos(t->t_box, (side - g.tile) / 2, (side - g.tile) / 2);
        lv_obj_set_style_transform_scale(t->t_box, 256 * side / g.tile, 0);
    }
}

/* One row on screen (XXL): nothing comes forward - there is no room and,
 * with one cover, no need. Two or more across get a ring instead. */
static bool ring_mode(void) { return rows_on_screen() == 1; }

static void tile_geometry(tile_t * t, bool focused)
{
    place_tile(t, focused ? g.tile_focus : g.tile);
}

/* The thumbnail size index for a pixel size: the first cached size >= px,
 * else the biggest. */
static int thumb_index_for(int px)
{
    for(int i = 0; i < LIB_THUMB_COUNT; i++) if(lib_thumb_px[i] >= px) return i;
    return LIB_THUMB_COUNT - 1;
}

static void tile_apply_bitmap(tile_t * t, bmp_t * b)
{
    lv_image_set_src(t->img, &b->dsc);
    lv_obj_remove_flag(t->img, LV_OBJ_FLAG_HIDDEN);
    t->shown_px = b->px;
    if(t->t_title) lv_obj_add_flag(t->t_title, LV_OBJ_FLAG_HIDDEN);
    if(t->t_artist) lv_obj_add_flag(t->t_artist, LV_OBJ_FLAG_HIDDEN);
    if(t->t_extra) lv_obj_add_flag(t->t_extra, LV_OBJ_FLAG_HIDDEN);
}

/* The best bitmap we hold for a tile: the focused size when focused, else
 * the tile size, else whatever is there. */
static void tile_refresh_bitmap(tile_t * t)
{
    const lib_album_t * a = album_of(t->entry);
    if(!a->has_art) return;
    int want = (t->entry == sel && !ring_mode()) ? g.tile_focus : g.tile;
    bmp_t * b = bmp_find(t->entry, want);
    if(!b) b = bmp_find(t->entry, t->entry == sel ? g.tile : g.tile_focus);
    if(b) { if(t->shown_px != b->px || lv_image_get_src(t->img) != &b->dsc) tile_apply_bitmap(t, b); }
}

/* The no-art text (0.16b, Ian 8 Oct): centred; whole words only, shrinking
 * the type down a ladder of sizes rather than breaking a word across lines;
 * a bracketed tail "(Deluxe Edition)" / "[Disc 2]" set smaller underneath;
 * a little variety in the typeface, steady per album. Only when even the
 * smallest step of the ladder will not take it does it wrap and trail off
 * with dots. */

#define LADDER_STEPS 9
#define MAIN_TRIES   6

static int text_w(const lv_font_t * f, const char * s)
{
    lv_point_t sz;
    lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

static const lv_font_t * face_px(const char * file, int px)
{
    return file ? ui_font_face_px(file, px, true) : ui_font_px(px);
}

/* Greedy word wrap into `out` (lines joined by '\n'); *whole is false when
 * a single word is wider than maxw. Returns the line count. */
static int wrap_words(const lv_font_t * f, const char * s, int maxw, char * out, size_t outlen, bool * whole)
{
    int lines = 0, linew = 0;
    size_t used = 0;
    int space_w = text_w(f, " ");
    *whole = true;
    out[0] = 0;
    const char * p = s;
    while(*p) {
        while(*p == ' ') p++;
        if(!*p) break;
        char word[160];
        size_t n = 0;
        while(p[n] && p[n] != ' ' && n < sizeof(word) - 1) n++;
        memcpy(word, p, n);
        word[n] = 0;
        p += n;
        int ww = text_w(f, word);
        if(ww > maxw) *whole = false;
        if(lines == 0) { lines = 1; linew = ww; }
        else if(linew + space_w + ww <= maxw) { if(used + 1 < outlen) out[used++] = ' '; linew += space_w + ww; }
        else { if(used + 1 < outlen) out[used++] = '\n'; lines++; linew = ww; }
        size_t wl = strlen(word);
        if(used + wl < outlen) { memcpy(out + used, word, wl); used += wl; }
        out[used] = 0;
    }
    return lines;
}

typedef struct {
    const lv_font_t * font;
    int  px, lines, h;
    bool whole;                 /* every word on one line, nothing trimmed */
    char text[400];
} fit_t;

static void ladder_fill(int * lad, int start)
{
    lad[0] = start;
    for(int i = 1; i < LADDER_STEPS; i++) { lad[i] = lad[i - 1] * 86 / 100; if(lad[i] < 10) lad[i] = 10; }
}

/* The largest rung from `from` on (to `last`) where `text` sits in maxw x maxh
 * in whole words and at most max_lines lines. Returns the rung, or -1. */
static int fit_rung(const char * file, const int * lad, int from, int last, const char * text,
                    int maxw, int maxh, int max_lines, fit_t * out)
{
    for(int k = from; k <= last; k++) {
        const lv_font_t * f = face_px(file, lad[k]);
        bool whole;
        int lines = wrap_words(f, text, maxw, out->text, sizeof(out->text), &whole);
        int h = lines * lv_font_get_line_height(f);
        if(k > from && lad[k] == lad[k - 1]) break;            /* the ladder has bottomed out */
        if(whole && lines <= max_lines && h <= maxh) {
            out->font = f; out->px = lad[k]; out->lines = lines; out->h = h; out->whole = true;
            return k;
        }
    }
    return -1;
}

/* A line of the no-art text, inside the tile's centring box (a flex column
 * that fills the tile, whatever its size: the text stays centred when the
 * tile grows under the cursor). `top` is the space above the line. */
static lv_obj_t * tile_label(lv_obj_t * box, const fit_t * ft, int w, int top)
{
    lv_obj_t * l = lv_label_create(box);
    lv_obj_set_style_text_font(l, ft->font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, ft->whole ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, ft->h);
    lv_obj_set_style_margin_top(l, top, 0);
    lv_label_set_text(l, ft->text);
    return l;
}

/* Split "Title (Deluxe Edition)" at its first bracket; returns false when
 * there is nothing before it or nothing in it. */
static bool split_bracket(const char * title, char * main, size_t mlen, char * br, size_t blen)
{
    const char * q = NULL;
    for(const char * p = title; *p; p++) if(*p == '(' || *p == '[') { q = p; break; }
    if(!q || q == title) return false;
    size_t n = (size_t)(q - title);
    while(n > 0 && title[n - 1] == ' ') n--;
    if(n == 0 || n >= mlen) return false;
    memcpy(main, title, n);
    main[n] = 0;
    snprintf(br, blen, "%s", q);
    return br[0] != 0;
}

static void caps_inplace(char * s)
{
    for(; *s; s++) if(*s >= 'a' && *s <= 'z') *s -= 32;
}

static void tile_make_text(tile_t * t)
{
    const lib_album_t * a = album_of(t->entry);
    int side = g.tile;
    int pad = side / 10;
    int w = side - 2 * pad, avail = side - 2 * pad;
    bool big = side >= BIG_TILE_PX;

    /* the typeface: steady per album, a little variety across the screen */
    unsigned hsh = 2166136261u;
    for(const char * p = a->title; *p; p++) hsh = (hsh ^ (unsigned char)*p) * 16777619u;
    bool caps_t = false, caps_a = false;
    const char * file_t;
    switch((hsh >> 8) % 3) {
        case 0:  file_t = placeholder_line_font(2, &caps_t); break;                         /* the tape's album line */
        case 1:  file_t = placeholder_line_font(3, &caps_t); break;                         /* mixed case            */
        default: file_t = "Caprasimo-Regular.ttf"; caps_t = false; break;                  /* a display face        */
    }
    const char * file_a = placeholder_line_font(1, &caps_a);

    char title[300], main[300], br[300], artist[300];
    snprintf(title, sizeof(title), "%s", a->title);
    snprintf(artist, sizeof(artist), "%s", artist_text(a));
    bool has_br = split_bracket(title, main, sizeof(main), br, sizeof(br));
    if(!has_br) snprintf(main, sizeof(main), "%s", title);
    if(caps_t) { caps_inplace(main); if(has_br) caps_inplace(br); }
    if(caps_a) caps_inplace(artist);

    int gap = pad / 2;
    /* artist: up to two lines on its own, shrinking by whole words */
    fit_t fa = { 0 };
    int used_h = 0;
    if(big && artist[0]) {
        int lad[LADDER_STEPS];
        ladder_fill(lad, side / 12 < 12 ? 12 : side / 12 > 34 ? 34 : side / 12);
        if(fit_rung(file_a, lad, 0, 4, artist, w, avail / 3, 2, &fa) < 0) {
            fa.font = face_px(file_a, lad[4]); fa.px = lad[4]; fa.whole = false;
            snprintf(fa.text, sizeof(fa.text), "%s", artist);
            fa.lines = 1; fa.h = lv_font_get_line_height(fa.font);
        }
        used_h = fa.h + gap;
    }

    /* title: the biggest rung where the main part and its bracket both fit */
    int lad[LADDER_STEPS];
    int start = side / 6;
    if(start < 16) start = 16;
    if(start > 72) start = 72;
    ladder_fill(lad, start);
    int room = avail - used_h;
    int max_lines = big ? 5 : 4;
    fit_t fm = { 0 }, fb = { 0 };
    bool placed = false;
    for(int k = 0; k < MAIN_TRIES && !placed; k++) {
        if(fit_rung(file_t, lad, k, k, main, w, room, max_lines, &fm) < 0) continue;
        if(!has_br) { placed = true; break; }
        int kb = k + 3 > LADDER_STEPS - 1 ? LADDER_STEPS - 1 : k + 3;
        if(fit_rung(file_t, lad, kb, LADDER_STEPS - 1, br, w, room - fm.h - gap / 2, 2, &fb) >= 0) { placed = true; break; }
    }
    if(!placed) {
        /* nothing fits whole: the biggest rung whose max_lines lines fit the
         * room, wrapped by LVGL, trailing off */
        int k = MAIN_TRIES - 1;
        for(int j = 2; j < MAIN_TRIES; j++)
            if(lv_font_get_line_height(face_px(file_t, lad[j])) * max_lines <= room) { k = j; break; }
        fm.font = face_px(file_t, lad[k]); fm.px = lad[k]; fm.whole = false;
        snprintf(fm.text, sizeof(fm.text), "%s", has_br ? title : main);
        if(has_br && caps_t) caps_inplace(fm.text);
        int lh = lv_font_get_line_height(fm.font);
        int lines = room / lh;
        if(lines < 1) lines = 1;
        if(lines > max_lines) lines = max_lines;
        fm.lines = lines; fm.h = lines * lh;
        has_br = false;
    }

    lv_obj_t * box = lv_obj_create(t->obj);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(box, g.tile, g.tile);
    lv_obj_set_style_transform_pivot_x(box, g.tile / 2, 0);
    lv_obj_set_style_transform_pivot_y(box, g.tile / 2, 0);
    t->t_box = box;
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if(used_h) t->t_artist = tile_label(box, &fa, w, 0);
    t->t_title = tile_label(box, &fm, w, used_h ? gap : 0);
    if(has_br) t->t_extra = tile_label(box, &fb, w, gap / 2);
}

/* The no-art colours (0.16f, Ian 9 Oct: more variety). Dealt per visit by
 * tilestyle.c from the theme's tilecolours setting - Ian's colour sets, a
 * mix of them, or colourways generated from them - so a screenful uses each
 * colour once before repeating and no tile matches the one left or above.
 * Held while in the grid: the same seed, entries and columns deal the same
 * colours again, so paging back shows what it showed before. */
static tile_style_t * styles;
static int            styles_n;

static void deal_colours(void)
{
    free(styles);
    styles = NULL;
    styles_n = 0;
    if(nentries == 0) return;
    styles = calloc((size_t)nentries, sizeof(*styles));
    bool * noart = calloc((size_t)nentries, sizeof(bool));
    if(styles && noart) {
        for(int e = 0; e < nentries; e++) noart[e] = !album_of(e)->has_art;
        tilestyle_deal(visit_seed, nentries, cols(), noart, styles);
        styles_n = nentries;
    }
    free(noart);
}

static uint32_t tile_colour(int entry, uint32_t * ink)
{
    const lib_album_t * a = album_of(entry);
    if(a->has_art && a->colour) { if(ink) *ink = 0xFFFFFF; return a->colour; }
    uint32_t bg = 0x333333, fg = 0xEEEEEE;
    if(entry >= 0 && entry < styles_n) { bg = styles[entry].bg; fg = styles[entry].ink; }
    if(ink) *ink = fg;
    return bg;
}

static tile_t * tile_create(int entry)
{
    tile_t * t = NULL;
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].entry < 0) { t = &tiles[i]; break; }
    if(!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->entry = entry;
    const lib_album_t * a = album_of(entry);
    uint32_t ink;
    uint32_t bg = tile_colour(entry, &ink);

    t->obj = lv_obj_create(field);
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
    lv_obj_set_size(t->img, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(t->img, 0, 0);
    lv_image_set_inner_align(t->img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_add_flag(t->img, LV_OBJ_FLAG_HIDDEN);

    if(!a->has_art) tile_make_text(t);
    tile_geometry(t, false);
    tile_refresh_bitmap(t);
    return t;
}

static void tile_delete(tile_t * t)
{
    if(t->obj) lv_obj_delete(t->obj);
    memset(t, 0, sizeof(*t));
    t->entry = -1;
}

static void tiles_clear(void)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].entry >= 0) tile_delete(&tiles[i]);
}

/* ------------------------------------------------------------- focus */

static void zoom_exec(void * obj, int32_t v)
{
    tile_t * t = NULL;
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].obj == obj) { t = &tiles[i]; break; }
    if(!t) return;
    place_tile(t, g.tile + (g.tile_focus - g.tile) * v / 100);
}

static void focus_tile(tile_t * t, bool on, bool animate)
{
    lv_anim_delete(t->obj, zoom_exec);
    if(ring_mode()) {
        int gap = g.cell - g.tile;
        int ring = cols() > 1 ? (gap / 2 < 3 ? 3 : gap / 2 > 8 ? 8 : gap / 2) : 0;
        lv_obj_set_style_shadow_width(t->obj, 0, 0);
        lv_obj_set_style_outline_width(t->obj, on ? ring : 0, 0);
        tile_geometry(t, false);
        tile_refresh_bitmap(t);
        return;
    }
    lv_obj_set_style_outline_width(t->obj, 0, 0);
    lv_obj_set_style_shadow_width(t->obj, on ? g.tile / 6 : 0, 0);
    if(on) {
        lv_obj_move_foreground(t->obj);
        if(animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, t->obj);
            lv_anim_set_exec_cb(&a, zoom_exec);
            lv_anim_set_values(&a, 0, 100);
            lv_anim_set_duration(&a, ZOOM_MS);
            lv_anim_start(&a);
        }
        else tile_geometry(t, true);
    }
    else tile_geometry(t, false);
    tile_refresh_bitmap(t);
}

/* ------------------------------------------------------------ window */

static void want_update(void);

/* Make sure the tiles for rows a..b (inclusive) exist, drop the rest. */
static void window_set(int a, int b)
{
    if(nentries == 0) { tiles_clear(); return; }
    if(a < 0) a = 0;
    if(b > rows_total - 1) b = rows_total - 1;
    int lo = a * cols(), hi = (b + 1) * cols() - 1;
    if(hi > nentries - 1) hi = nentries - 1;
    for(int i = 0; i < MAX_TILES; i++)
        if(tiles[i].entry >= 0 && (tiles[i].entry < lo || tiles[i].entry > hi)) tile_delete(&tiles[i]);
    for(int e = lo; e <= hi; e++) if(!tile_of(e)) tile_create(e);
    tile_t * ts = tile_of(sel);
    if(ts) focus_tile(ts, true, false);
}

static void window_normal(void)
{
    window_set(first_row - 1, first_row + rows_on_screen());
    want_update();
}

/* The decoder's wanted list: what is on screen (the selected cover sharp
 * first), then one row either side, then the next page in the direction
 * of travel. */
static int travel_dir = 1;

static void want_update(void)
{
    static thumb_req_t reqs[THUMBS_MAX_WANT];
    int n = 0;
    if(nentries == 0) { thumbs_want(reqs, 0); return; }
    int lo = first_row * cols(), hi = (first_row + rows_on_screen()) * cols() - 1;
    int ahead_lo = travel_dir > 0 ? hi + 1 : lo - rows_on_screen() * cols();
    int ahead_hi = travel_dir > 0 ? hi + rows_on_screen() * cols() : lo - 1;
    int order[4][2] = { { sel, sel }, { lo, hi }, { lo - cols(), lo - 1 }, { hi + 1, hi + cols() } };
    for(int k = 0; k < 5 && n < THUMBS_MAX_WANT; k++) {
        int a = k < 4 ? order[k][0] : ahead_lo, b = k < 4 ? order[k][1] : ahead_hi;
        for(int e = a; e <= b && n < THUMBS_MAX_WANT; e++) {
            if(e < 0 || e >= nentries) continue;
            const lib_album_t * al = album_of(e);
            if(!al->has_art) continue;
            int px = (k == 0 && !ring_mode()) ? g.tile_focus : g.tile;
            if(bmp_find(e, px)) continue;
            int si = thumb_index_for(px);
            if(!(al->thumb_sizes & (1 << si))) {
                /* that size not cached: the biggest that is */
                int found = -1;
                for(int i = LIB_THUMB_COUNT - 1; i >= 0; i--) if(al->thumb_sizes & (1 << i)) { found = i; break; }
                if(found < 0) continue;
                si = found;
            }
            bool dup = false;
            for(int i = 0; i < n; i++) if(reqs[i].id == e && reqs[i].px == px) { dup = true; break; }
            if(dup) continue;
            reqs[n].id = e;
            reqs[n].gen = list_gen;
            reqs[n].px = px;
            cache_thumb_path(sources[entries[e].src].card, al, si, reqs[n].path, sizeof(reqs[n].path));
            n++;
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
        if(r.gen != list_gen || r.id < 0 || r.id >= nentries) { free(r.pixels); continue; }
        bmp_t * old = bmp_find(r.id, r.px);
        if(old) { free(r.pixels); continue; }
        bmp_add(r.id, r.px, r.pixels, r.w, r.h);
        tile_t * t = tile_of(r.id);
        if(t) tile_refresh_bitmap(t);
    }
}

/* ------------------------------------------------------------ names */

static void namebar_hide_cb(lv_timer_t * tm)
{
    (void)tm;
    lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    namebar_timer = NULL;
}

/* "Artist – Title" with a bracketed tail "(Deluxe Edition)" in a smaller
 * face after it (Ian, 8 Oct). Two labels in a flex row; the main one gives
 * way (dots) when the two will not fit side by side. */
static void nlabel_create(nlabel_t * o, lv_obj_t * parent, uint32_t colour, bool stroked, lv_label_long_mode_t mode)
{
    o->n = stroked ? 9 : 1;
    if(stroked) {
        o->box = lv_obj_create(parent);
        lv_obj_remove_style_all(o->box);
        lv_obj_remove_flag(o->box, LV_OBJ_FLAG_SCROLLABLE);
    }
    for(int i = 0; i < o->n; i++) {
        o->l[i] = lv_label_create(stroked ? o->box : parent);
        lv_obj_set_style_text_color(o->l[i], i == o->n - 1 ? lv_color_hex(colour) : lv_color_black(), 0);
        lv_label_set_long_mode(o->l[i], mode);
        lv_label_set_text(o->l[i], "");
    }
    if(!stroked) o->box = o->l[0];
}

static void nlabel_set(nlabel_t * o, const lv_font_t * f, const char * text, int w, int h, int pad_left, int pad_bottom)
{
    int step = o->n > 1 ? (lv_font_get_line_height(f) / 14 < 1 ? 1 : lv_font_get_line_height(f) / 14) : 0;
    static const int off[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for(int i = 0; i < o->n; i++) {
        lv_obj_set_style_text_font(o->l[i], f, 0);
        lv_label_set_text(o->l[i], text);
        lv_obj_set_size(o->l[i], w, h);
        if(o->n > 1) lv_obj_set_pos(o->l[i], step + (i < 8 ? off[i][0] * step : 0), step + (i < 8 ? off[i][1] * step : 0));
    }
    if(o->n > 1) {
        lv_obj_set_size(o->box, w + 2 * step, h + 2 * step);
        lv_obj_set_style_pad_left(o->box, pad_left, 0);
        lv_obj_set_style_pad_bottom(o->box, pad_bottom, 0);
        lv_obj_set_width(o->box, w + 2 * step + pad_left);
    }
    else {
        lv_obj_set_style_pad_left(o->box, pad_left, 0);
        lv_obj_set_style_pad_bottom(o->box, pad_bottom, 0);
        lv_obj_set_width(o->box, w + pad_left);
    }
}

/* "Artist – Title" with a bracketed tail "(Deluxe Edition)" in a smaller
 * face after it (Ian, 8 Oct). Two labels in a flex row; the main one gives
 * way (dots) when the two will not fit side by side. */
static void name_fill(nlabel_t * main, nlabel_t * tail, int box_w, int px, const lib_album_t * a)
{
    char text[700], mainpart[300], br[300];
    const char * ar = artist_text(a);
    bool has_br = split_bracket(a->title, mainpart, sizeof(mainpart), br, sizeof(br));
    if(ar[0]) snprintf(text, sizeof(text), "%s \xE2\x80\x93 %s", ar, has_br ? mainpart : a->title);
    else snprintf(text, sizeof(text), "%s", has_br ? mainpart : a->title);
    const lv_font_t * fm = ui_font_px(px);
    const lv_font_t * ft = ui_font_px(px * 3 / 4 < 10 ? 10 : px * 3 / 4);
    int gap = px / 3;
    int tail_w = has_br ? text_w(ft, br) : 0;
    if(tail_w > box_w / 2) tail_w = box_w / 2;
    int room = box_w - tail_w - (has_br ? gap : 0);
    int mw = text_w(fm, text);
    /* one line, fixed, so the dots mode trims instead of wrapping; the tail's
     * baseline sits a little above the main's */
    nlabel_set(main, fm, text, mw < room ? mw : room, lv_font_get_line_height(fm), 0, 0);
    nlabel_set(tail, ft, has_br ? br : "", tail_w, lv_font_get_line_height(ft), has_br ? gap : 0,
               (lv_font_get_line_height(fm) - lv_font_get_line_height(ft)) / 5);
}

static void show_name(void)
{
    if(nentries == 0) return;
    const lib_album_t * a = album_of(sel);
    if(g.band_h) { name_fill(&band_main, &band_tail, g.w - 2 * mm.margin, mm.font_px, a); return; }
    if(picker_mode() == PICKER_STRIP) return;              /* the pill is driving: no subtitle under it */
    name_fill(&name_main, &name_tail, g.w - 2 * mm.margin, mm.font_px, a);
    lv_obj_remove_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(namebar);
    if(namebar_timer) lv_timer_reset(namebar_timer);
    else { namebar_timer = lv_timer_create(namebar_hide_cb, NAMEBAR_MS, NULL); lv_timer_set_repeat_count(namebar_timer, 1); }
}

/* ------------------------------------------------------------ scroll */

static void scroll_exec(void * obj, int32_t v) { lv_obj_scroll_to_y(obj, v, LV_ANIM_OFF); }

static void scroll_done_cb(lv_anim_t * a)
{
    (void)a;
    window_normal();
}

/* Scroll so `row` is the top row: animated, with the tiles of both the old
 * and the new rows present for the slide. */
static void scroll_to_row(int row, int ms)
{
    if(row < 0) row = 0;
    if(row > rows_total - rows_on_screen()) row = rows_total - rows_on_screen();
    if(row < 0) row = 0;
    int from = first_row;
    first_row = row;
    int far = from > row ? from - row : row - from;
    if(far > rows_on_screen()) ms = 0;                 /* a jump, not a slide */
    int lo = (from < row ? from : row) - 1, hi = (from > row ? from : row) + rows_on_screen();
    if(ms > 0) window_set(lo, hi);
    lv_anim_delete(field, scroll_exec);
    if(ms <= 0 || from == row) { lv_obj_scroll_to_y(field, row * g.cell, LV_ANIM_OFF); window_normal(); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, field);
    lv_anim_set_exec_cb(&a, scroll_exec);
    lv_anim_set_values(&a, lv_obj_get_scroll_y(field), row * g.cell);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, scroll_done_cb);
    lv_anim_start(&a);
}

/* Move the cursor to `e`, scrolling as needed. `page`: a whole-screen move. */
static void set_sel(int e, bool page)
{
    if(nentries == 0) return;
    if(e < 0) e = 0;
    if(e > nentries - 1) e = nentries - 1;
    int old = sel;
    travel_dir = e >= old ? 1 : -1;
    sel = e;
    tile_t * to = tile_of(old);
    if(to && old != e) focus_tile(to, false, false);

    int row = row_of(e);
    int top = first_row, bottom = first_row + rows_on_screen() - 1;
    bool pill = picker_mode() == PICKER_STRIP;
    if(row < top || row > bottom || pill) {
        int target = row < top ? row : row > bottom ? row - rows_on_screen() + 1 : first_row;
        if(pill && rows_on_screen() > 1) target = row - 1;          /* under the pill: one row down */
        if(target < 0) target = 0;
        if(target != first_row) scroll_to_row(target, page ? PAGE_SLIDE_MS : ROW_SLIDE_MS);
    }
    tile_t * tn = tile_of(e);
    if(tn) focus_tile(tn, true, old != e);
    want_update();
    show_name();
}

/* ------------------------------------------------------------ library */

static void entries_free(void)
{
    free(entries);
    entries = NULL;
    nentries = 0;
}

static int entry_cmp(const void * pa, const void * pb)
{
    const entry_t * a = pa, * b = pb;
    int c = library_album_cmp(&sources[a->src].lib.albums[a->album], &sources[b->src].lib.albums[b->album], sort_kind);
    if(!c) c = a->src - b->src;
    return c;
}

/* The pill's labels under this sort: letters of titles or artists, or
 * decades for the year sorts; "?" for the end section. */
static void build_labels(void)
{
    view_opts.label_count = 0;
    view_opts.jump = -1;
    if(nentries == 0) return;
    bool by_year = sort_kind == LIB_SORT_YEAR || sort_kind == LIB_SORT_ARTIST_YEAR;
    bool by_artist = sort_kind == LIB_SORT_ARTIST_TITLE || sort_kind == LIB_SORT_ARTIST_YEAR;
    if(sort_kind == LIB_SORT_NEWEST) { for(int e = 0; e < nentries; e++) entries[e].label = -1; return; }

    bool end_section = false;
    for(int e = 0; e < nentries; e++) if(library_album_lacks(album_of(e), sort_kind)) { end_section = true; break; }

    if(by_year && sort_kind == LIB_SORT_YEAR) {
        /* decades, in order of appearance (the list is sorted by year) */
        for(int e = 0; e < nentries; e++) {
            const lib_album_t * a = album_of(e);
            if(library_album_lacks(a, sort_kind)) { entries[e].label = -1; continue; }
            char d[PICKER_LABEL_LEN];
            snprintf(d, sizeof(d), "%d0s", (a->year / 10) % 1000);
            int idx = -1;
            for(int i = 0; i < view_opts.label_count; i++) if(strcmp(view_opts.labels[i], d) == 0) { idx = i; break; }
            if(idx < 0 && view_opts.label_count < PICKER_MAX_LABELS - 1) { idx = view_opts.label_count++; snprintf(view_opts.labels[idx], PICKER_LABEL_LEN, "%s", d); }
            entries[e].label = idx;
        }
    }
    else {
        const char ** names = malloc(sizeof(char *) * (size_t)(nentries > 0 ? nentries : 1));
        int n = 0;
        if(!names) return;
        for(int e = 0; e < nentries; e++) {
            const lib_album_t * a = album_of(e);
            if(library_album_lacks(a, sort_kind)) continue;
            names[n++] = by_artist ? artist_text(a) : a->title;
        }
        picker_labels_from_names(&view_opts, names, n, end_section);
        /* the pill steps through the letters in the order the list shows
         * them (the list is in byte order until the collation pass; the
         * picker groups by script), so reorder by first appearance */
        char ordered[PICKER_MAX_LABELS][PICKER_LABEL_LEN];
        int m = 0;
        for(int i = 0; i < n; i++) {
            int l = picker_label_of(&view_opts, names[i]);
            if(l < 0) continue;
            bool seen = false;
            for(int k = 0; k < m; k++) if(strcmp(ordered[k], view_opts.labels[l]) == 0) { seen = true; break; }
            if(!seen && m < PICKER_MAX_LABELS) snprintf(ordered[m++], PICKER_LABEL_LEN, "%s", view_opts.labels[l]);
        }
        if(end_section && m < PICKER_MAX_LABELS) snprintf(ordered[m++], PICKER_LABEL_LEN, "?");
        memcpy(view_opts.labels, ordered, sizeof(ordered));
        view_opts.label_count = m;
        free(names);
        for(int e = 0; e < nentries; e++) {
            const lib_album_t * a = album_of(e);
            entries[e].label = library_album_lacks(a, sort_kind) ? -1 : picker_label_of(&view_opts, by_artist ? artist_text(a) : a->title);
        }
    }
    if(end_section) {
        /* the "?" chip is the last label */
        int q = view_opts.label_count - 1;
        if(q >= 0 && strcmp(view_opts.labels[q], "?") == 0)
            for(int e = 0; e < nentries; e++) if(library_album_lacks(album_of(e), sort_kind)) entries[e].label = q;
    }
    view_opts.jump = view_opts.label_count ? 0 : -1;
}

static void sort_entries(void)
{
    if(nentries > 1) qsort(entries, (size_t)nentries, sizeof(entry_t), entry_cmp);
    build_labels();
}

static bool index_changed(void)
{
    for(int s = 0; s < nsources; s++) {
        struct stat st;
        if(stat(sources[s].idx_path, &st) != 0) return true;
        if(st.st_mtime != sources[s].idx_mtime) return true;
    }
    return false;
}

/* Load the index of every card MPD has mounted. Returns the entry (by
 * folder) to put the cursor on, or -1. */
static void load_library(const char * keep_folder)
{
    for(int s = 0; s < nsources; s++) library_free(&sources[s].lib);
    nsources = 0;
    entries_free();
    lib_loaded = false;

    mpd_listing_t root;
    if(!mpd_lsinfo("", &root)) return;
    lib_loaded = true;
    for(int i = 0; i < root.count && nsources < MAX_SOURCES; i++) {
        const mpd_entry_t * e = &root.items[i];
        if(e->kind != MPD_ENTRY_DIR || strncmp(e->uri, "card-", 5) != 0 || strchr(e->uri, '/')) continue;
        source_t * s = &sources[nsources];
        snprintf(s->card, sizeof(s->card), "%s", e->uri);
        cache_index_path(s->card, s->idx_path, sizeof(s->idx_path));
        struct stat st;
        if(stat(s->idx_path, &st) != 0) continue;
        if(!library_load(&s->lib, s->idx_path)) continue;
        s->idx_mtime = st.st_mtime;
        nsources++;
    }
    mpd_listing_free(&root);

    int total = 0;
    for(int s = 0; s < nsources; s++) total += sources[s].lib.nalbums;
    entries = total ? malloc(sizeof(entry_t) * (size_t)total) : NULL;
    for(int s = 0; s < nsources && entries; s++)
        for(int a = 0; a < sources[s].lib.nalbums; a++) {
            if(sources[s].lib.albums[a].in_collection) continue;        /* singles in a collection folder */
            entries[nentries].src = s;
            entries[nentries].album = a;
            entries[nentries].label = -1;
            nentries++;
        }
    sort_entries();
    rows_total = (nentries + cols() - 1) / cols();
    sel = 0;
    if(keep_folder) for(int e = 0; e < nentries; e++) if(strcmp(album_of(e)->folder, keep_folder) == 0) { sel = e; break; }
    fprintf(stderr, "simpleton-ui: grid: %d albums from %d card%s, %s, %d labels\n",
            nentries, nsources, nsources == 1 ? "" : "s", library_sort_name(sort_kind), view_opts.label_count);
}

/* ------------------------------------------------------------ layout */

static void measure(void)
{
    ui_grid_metrics(view_opts.size, &g);
    ui_list_metrics(UI_SIZE_DEFAULT, &mm);
    /* the picker sits over the field: its "list area" is the grid, the
     * pill in the top row band */
    plm = mm;
    plm.x = g.x;
    plm.y = g.y;
    plm.w = g.w;
    plm.h = g.h;
}

static void layout_screen(void)
{
    lv_obj_set_pos(field, g.x, g.y);
    lv_obj_set_size(field, g.w, g.h);
    lv_obj_set_size(spacer, 1, rows_total * g.cell);
    lv_obj_set_pos(spacer, 0, 0);
    if(g.band_h) {
        lv_obj_remove_flag(band, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(band, g.x + mm.margin, g.band_y);
        lv_obj_set_size(band, g.w - 2 * mm.margin, g.band_h);
        lv_obj_set_style_pad_top(band, (g.band_h - lv_font_get_line_height(ui_font_px(mm.font_px))) / 2, 0);
    }
    else lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    /* subtitle-style, toward the top (Ian, 8 Oct: a bar at the bottom sat
     * over what you were scrolling toward) */
    lv_obj_set_pos(namebar, g.x, g.y + mm.margin);
    lv_obj_set_size(namebar, g.w, mm.row_h);

    lv_obj_set_style_text_font(msg, ui_font_px(mm.font_px), 0);
    lv_obj_center(msg);
}

/* Draw the whole thing again (a new size, a new sort, a reload). */
static void rebuild(void)
{
    tiles_clear();
    bmp_clear_all();
    measure();
    rows_total = nentries ? (nentries + cols() - 1) / cols() : 0;
    deal_colours();
    layout_screen();
    picker_relayout(&plm);
    if(sel > nentries - 1) sel = nentries ? nentries - 1 : 0;
    first_row = row_of(sel) - rows_on_screen() / 2;
    if(first_row > rows_total - rows_on_screen()) first_row = rows_total - rows_on_screen();
    if(first_row < 0) first_row = 0;
    lv_obj_scroll_to_y(field, first_row * g.cell, LV_ANIM_OFF);
    if(nentries == 0) {
        lv_label_set_text(msg, T(lib_loaded ? S_GRID_EMPTY : S_BROWSER_STARTING));
        lv_obj_remove_flag(msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
        if(g.band_h) { lv_label_set_text(band_main.l[0], ""); lv_label_set_text(band_tail.l[0], ""); }
        return;
    }
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    window_normal();
    if(g.band_h) show_name();
}

/* ------------------------------------------------------------ picker */

static bool grid_is_long(void)
{
    return nentries > STRIP_SCREENS * rows_on_screen() * cols() && view_opts.label_count >= STRIP_MIN_LABELS;
}

static void on_jump(int label, void * ctx)
{
    (void)ctx;
    for(int e = 0; e < nentries; e++) if(entries[e].label == label) { set_sel(e, true); return; }
}

static void on_change(const picker_opts_t * o, void * ctx)
{
    (void)ctx;
    bool resize = o->size != view_opts.size;
    bool resort = o->sort != (int)sort_kind;
    view_opts.size = o->size;
    view_opts.strip = o->strip;
    view_opts.style = o->style;
    if(resort) {
        sort_kind = (lib_sort_t)o->sort;
        view_opts.sort = o->sort;
        const lib_album_t * keep = nentries ? album_of(sel) : NULL;
        char * folder = keep ? strdup(keep->folder) : NULL;
        sort_entries();
        if(folder) { for(int e = 0; e < nentries; e++) if(strcmp(album_of(e)->folder, folder) == 0) { sel = e; break; } free(folder); }
    }
    if(resize || resort) rebuild();
    write_options();
}

static void open_strip(void);

static void on_close(bool back, void * ctx)
{
    picker_mode_t was = (picker_mode_t)(intptr_t)ctx;
    mark_visited();
    if(back && picker_mode() == PICKER_NONE && was == PICKER_STRIP) { if(exit_cb) exit_cb(); return; }
    if(was == PICKER_FULL && view_opts.strip && grid_is_long() && !visited_before_open) open_strip();
    else { tile_t * t = tile_of(sel); if(t) focus_tile(t, true, false); }
}

static void open_strip(void)
{
    if(nentries == 0) return;
    int l = entries[sel].label;
    view_opts.jump = l >= 0 ? l : 0;
    visited_before_open = visited;
    picker_open(PICKER_STRIP, &view_opts, on_jump, on_change, on_close, (void *)(intptr_t)PICKER_STRIP);
}

static void open_full(void)
{
    int l = nentries ? entries[sel].label : -1;
    view_opts.jump = l >= 0 ? l : 0;
    visited_before_open = visited;
    picker_open(PICKER_FULL, &view_opts, on_jump, on_change, on_close, (void *)(intptr_t)PICKER_FULL);
}

static void after_enter(void)
{
    if(suppress_overlay) { suppress_overlay = false; return; }
    if(nentries == 0) return;
    if(!visited) { open_full(); return; }
    if(view_opts.strip && grid_is_long()) open_strip();
}

/* ------------------------------------------------------------ public */

static void index_timer_cb(lv_timer_t * tm)
{
    (void)tm;
    if(!shown) return;
    if(!lib_loaded || index_changed()) {
        const lib_album_t * keep = nentries ? album_of(sel) : NULL;
        char * folder = keep ? strdup(keep->folder) : NULL;
        load_library(folder);
        free(folder);
        rebuild();
    }
}

void grid_create(void (*on_open)(const char *, const char *), void (*on_exit)(void))
{
    open_cb = on_open;
    exit_cb = on_exit;
    for(int i = 0; i < MAX_TILES; i++) tiles[i].entry = -1;
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
    lv_obj_set_flex_flow(band, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(band, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    nlabel_create(&band_main, band, UI_COLOR_FG, false, LV_LABEL_LONG_MODE_DOTS);
    nlabel_create(&band_tail, band, UI_COLOR_DIM, false, LV_LABEL_LONG_MODE_CLIP);

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
    lv_obj_set_flex_flow(namebar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(namebar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_START);
    nlabel_create(&name_main, namebar, 0xFFFFFF, true, LV_LABEL_LONG_MODE_DOTS);
    nlabel_create(&name_tail, namebar, 0xDDDDDD, true, LV_LABEL_LONG_MODE_CLIP);

    msg = lv_label_create(scr);
    lv_obj_set_style_text_color(msg, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);

    layout_screen();
    poll_timer = lv_timer_create(poll_cb, 40, NULL);
    index_timer = lv_timer_create(index_timer_cb, 3000, NULL);
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
}

void grid_show(bool fresh)
{
    shown = true;
    if(fresh) visit_seed = (unsigned)time(NULL) ^ (unsigned)lv_tick_get() ^ (unsigned)rand();
    suppress_overlay = !fresh;                 /* back from an album: no pill, the cursor on it */
    read_options();
    lv_screen_load(scr);
    picker_attach(scr, &plm);
    lv_timer_resume(poll_timer);
    lv_timer_resume(index_timer);
    if(!lib_loaded || index_changed()) {
        const lib_album_t * keep = nentries ? album_of(sel) : NULL;
        char * folder = keep ? strdup(keep->folder) : NULL;
        load_library(folder);
        free(folder);
    }
    rebuild();
    after_enter();
}

void grid_hide(void)
{
    shown = false;
    if(picker_mode() != PICKER_NONE) picker_close();
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
    thumbs_want(NULL, 0);
    tiles_clear();
    bmp_clear_all();
}

bool grid_overlay_active(void) { return shown && picker_mode() != PICKER_NONE; }

void grid_toggle_picker(void)
{
    if(!shown) return;
    if(picker_mode() != PICKER_NONE) { picker_toggle_panel(); return; }
    open_full();
}

void grid_reload_options(void)
{
    ui_size_t old_size = view_opts.size;
    lib_sort_t old_sort = sort_kind;
    read_options();
    if(!shown) return;
    if(sort_kind != old_sort) {
        const lib_album_t * keep = nentries ? album_of(sel) : NULL;
        char * folder = keep ? strdup(keep->folder) : NULL;
        sort_entries();
        if(folder) { for(int e = 0; e < nentries; e++) if(strcmp(album_of(e)->folder, folder) == 0) { sel = e; break; } free(folder); }
    }
    if(view_opts.size != old_size || sort_kind != old_sort) rebuild();
    if(!view_opts.strip && picker_mode() == PICKER_STRIP) picker_close();
}

void grid_library_changed(void)
{
    if(shown) lv_timer_ready(index_timer);
}

void grid_handle_action(ui_action_t a)
{
    if(picker_mode() != PICKER_NONE) { picker_handle_action(a); return; }
    if(nentries == 0) { if(a == ACT_BACK && exit_cb) exit_cb(); return; }
    int c = cols(), r = row_of(sel), k = col_of(sel);
    switch(a) {
        /* the ends wrap (Ian, 8 Oct): Up from the top row lands on the
         * bottom row, Down from the bottom on the top, Left from the first
         * album on the last and Right from the last on the first */
        case ACT_UP:    if(r > 0) set_sel(sel - c, false);
                        else { int e = (rows_total - 1) * c + k; set_sel(e < nentries ? e : nentries - 1, false); }
                        break;
        case ACT_DOWN:  if(sel + c < nentries) set_sel(sel + c, false);
                        else if(r < rows_total - 1) set_sel(nentries - 1, false);
                        else set_sel(k, false);
                        break;
        case ACT_LEFT:
            if(k > 0) set_sel(sel - 1, false);
            else if(sel == 0) set_sel(nentries - 1, false);
            else if(r > 0) {
                /* off the left edge: a page back, the cursor keeping its spot */
                int page = rows_on_screen() * c;
                int target = sel - page;
                if(target < 0) target = k;                       /* the top page: same column, first row */
                int new_first = first_row - rows_on_screen();
                if(new_first < 0) new_first = 0;
                travel_dir = -1;
                scroll_to_row(new_first, PAGE_SLIDE_MS);
                set_sel(target, true);
            }
            break;
        case ACT_RIGHT:
            if(k < c - 1 && sel + 1 < nentries) set_sel(sel + 1, false);
            else if(sel + 1 >= nentries) set_sel(0, false);
            else {
                int page = rows_on_screen() * c;
                int target = sel + page;
                if(target > nentries - 1) target = nentries - 1;
                int new_first = first_row + rows_on_screen();
                if(new_first > rows_total - rows_on_screen()) new_first = rows_total - rows_on_screen();
                if(new_first < 0) new_first = 0;
                travel_dir = 1;
                scroll_to_row(new_first, PAGE_SLIDE_MS);
                set_sel(target, true);
            }
            break;
        case ACT_SELECT: {
            const lib_album_t * al = album_of(sel);
            if(open_cb) open_cb(al->folder, al->first_uri);
            break;
        }
        case ACT_BACK:
            if(view_opts.strip && grid_is_long()) open_strip();
            else if(exit_cb) exit_cb();
            break;
        default: break;
    }
}
