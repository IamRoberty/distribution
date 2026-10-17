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
#include "power.h"
#include "art.h"
#include "artists.h"
#include "curation.h"
#include "objmenu.h"
#include "cache.h"
#include "config.h"
#include "library.h"
#include "libs.h"
#include "mpdc.h"
#include "playthrough.h"
#include "picker.h"
#include "placeholder.h"
#include "strings.h"
#include "theme.h"
#include "thumbs.h"
#include "tilestyle.h"
#include "textfit.h"
#include "tiletext.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop, not in lvgl.h */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_TILES     160                     /* (rows + 2) * cols, twice, on the biggest grid */
#define BMP_MAX       256
#define BMP_BUDGET    (32u * 1024u * 1024u)   /* bytes of decoded thumbnails kept */
#define NAMEBAR_MS    1400
#define ZOOM_MS       100
#define ROW_SLIDE_MS  110
#define PAGE_SLIDE_MS 160
#define STRIP_SCREENS 3                       /* the pill past this many screenfuls... */
#define STRIP_MIN_LABELS 3                    /* ...with at least this many letters   */

/* ---- the library ---- */

static int find_key(const char * key);

/* What a tile stands for (0.19): an album, a track (the Tracks view's
 * grid), or a folder (an artist or library folder's subfolders that aren't
 * albums - a name tile with a keyline, or the folder's own picture). */
typedef enum { ENTRY_ALBUM, ENTRY_TRACK, ENTRY_FOLDER, ENTRY_ARTIST } entry_kind_t;

typedef struct {
    int    src;
    entry_kind_t kind;
    int    album;             /* the album (a track's album; -1 for a folder) */
    int    track;             /* ENTRY_TRACK: index into the source's tracks */
    int    artist;            /* ENTRY_ARTIST: index in artists.h (0.20)        */
    char * name;              /* ENTRY_FOLDER / ENTRY_ARTIST: the name shown     */
    char * uri;               /* ENTRY_FOLDER: its URI                          */
    char * pic;               /* ENTRY_FOLDER / ENTRY_ARTIST: a picture file, or NULL */
    long   newest;            /* ENTRY_FOLDER: its newest album's "added" (0.19a) */
    int    label;             /* index into view_opts.labels, -1 none           */
} entry_t;

static entry_t * entries;
static int       nentries;
static unsigned  lib_gen;          /* libs_generation() the entries were built from */
static unsigned  cur_gen;          /* curation_generation() a collection's entries were built from (0.21) */
#define SORT_AS_ADDED LIB_SORT_COUNT   /* a collection in its own order (0.21) */

/* The scope (0.19): every album, one folder's children, or every track. */
static grid_scope_t scope = GRID_SCOPE_ALBUMS;
static char         scope_folder[1024];
static void (*open_folder_cb)(const char * uri);
static void (*open_artist_cb)(int artist);
static void (*play_cb)(void);
static void (*style_cb)(const char * style);
static bool  scope_changed = true;
static bool  mixed_kinds;          /* albums and folders side by side: folder tiles get a keyline */

/* ---- options ---- */

static picker_opts_t view_opts;
static lib_sort_t    sort_kind;
/* The Sort row's choices for this scope, in the row's order (0.20; the
 * folder scope's "Newest" had been read back as the wrong sort). */
static lib_sort_t    sort_map[PICKER_MAX_CHOICES];
static int           sort_rows;
/* The Artists scope has "Opens as" in the Sort row's place: what Select on
 * an artist opens (main.c reads the same setting). */
static int           opens;
static bool          visited, visited_before_open;
static bool          suppress_overlay;
static bool          returning;            /* grid_show(false): back from a deeper view, the rows as they were */
/* Where each grid was left (0.23c, Ian, 17 Oct: a folder's grid came back
 * on the bottom row with its colours re-dealt): by scope and folder - the
 * deal's seed, the cursor and the top row, restored when that grid comes
 * back. The last eight. */
typedef struct { grid_scope_t scope; char folder[1024]; unsigned seed; int sel, first_row; bool used; } grid_memory_t;
static grid_memory_t memories[8];
static void memory_store(void);
static const grid_memory_t * memory_find(void)
{
    for(int i = 0; i < 8; i++) if(memories[i].used && memories[i].scope == scope && strcmp(memories[i].folder, scope_folder) == 0) return &memories[i];
    return NULL;
}

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

static void (*open_cb)(const library_t *, int, const char *, const tile_style_t *);
static void (*exit_cb)(void);

/* ---- tiles ---- */

typedef struct {
    int        entry;                 /* -1 = free                                  */
    lv_obj_t * obj;                   /* the tile: bg colour, shadow when focused   */
    lv_obj_t * img;                   /* the cover, hidden until a bitmap arrives   */
    lv_obj_t * t_box;                 /* no-art text box, NULL when the album has art; scaled with the tile */
    lv_obj_t * t_title, * t_artist, * t_extra;   /* its lines (t_extra: the bracketed tail) */
    lv_obj_t * t_keyline;             /* a folder tile's frame (0.19), or NULL      */
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

static const library_t * lib_of(int e) { return &libs_get(entries[e].src)->lib; }
static const char * card_of(int e) { return libs_get(entries[e].src)->card; }

/* The album a tile shows (a track's album), or NULL for a folder tile. */
static const lib_album_t * album_of(int e)
{
    if(e < 0 || e >= nentries || entries[e].album < 0) return NULL;
    return &lib_of(e)->albums[entries[e].album];
}

static const lib_track_t * track_of(int e)
{
    return entries[e].kind == ENTRY_TRACK ? &lib_of(e)->tracks[entries[e].track] : NULL;
}

static bool entry_has_art(int e)
{
    if(entries[e].kind == ENTRY_FOLDER || entries[e].kind == ENTRY_ARTIST) return entries[e].pic != NULL;
    const lib_album_t * a = album_of(e);
    return a && a->has_art;
}

static const char * artist_text(const lib_album_t * a);

/* The two lines a tile or the name band shows. */
static const char * entry_title(int e)
{
    const lib_track_t * t = track_of(e);
    if(t) return t->title[0] ? t->title : (album_of(e) ? album_of(e)->title : "");
    if(entries[e].kind == ENTRY_FOLDER || entries[e].kind == ENTRY_ARTIST) return entries[e].name;
    return album_of(e)->title;
}

static const char * entry_artist(int e)
{
    const lib_track_t * t = track_of(e);
    if(t) return t->artist[0] ? t->artist : (album_of(e) ? artist_text(album_of(e)) : "");
    if(entries[e].kind == ENTRY_FOLDER || entries[e].kind == ENTRY_ARTIST) return "";
    return artist_text(album_of(e));
}

/* The release tail an album carries ("2019 Remaster"), an artist's album
 * count, "" otherwise. */
static const char * entry_release(int e)
{
    if(entries[e].kind == ENTRY_ARTIST) {
        static char n[64];
        return artists_count_text(entries[e].artist, opens != 0, n, sizeof(n));
    }
    const lib_album_t * a = entries[e].kind == ENTRY_ALBUM ? album_of(e) : NULL;
    return a && a->release ? a->release : "";
}

static int cols(void) { return g.cols; }
static int rows_on_screen(void) { return g.rows; }
static int row_of(int e) { return e / cols(); }
static int col_of(int e) { return e % cols(); }

/* Settings are kept per view: albums, tracks, or the folder views
 * (artist and library folders share one set). */
static const char * view_id(void)
{
    switch(scope) {
        case GRID_SCOPE_TRACKS:  return "tracks";
        case GRID_SCOPE_FOLDER:  return "folder";
        case GRID_SCOPE_ARTISTS: return "artists";    /* shared with rows.c and the list page: one style key */
        case GRID_SCOPE_ARTIST:  return "artist";
        case GRID_SCOPE_COLLECTION: return "collection";
        default:                 return "albums";
    }
}

static void view_key(const char * what, char * out, size_t len)
{
    snprintf(out, len, "%s-%s-%s", what, view_id(), ui_screen_kind_name());
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

static lib_sort_t read_sort(lib_sort_t dflt)
{
    char key[64], v[24];
    view_key("sort", key, sizeof(key));
    config_read(key, v, sizeof(v), dflt == SORT_AS_ADDED ? "added" : library_sort_name(dflt));
    if(strcmp(v, "added") == 0) return SORT_AS_ADDED;
    for(int i = 0; i < LIB_SORT_COUNT; i++)
        if(strcmp(library_sort_name((lib_sort_t)i), v) == 0) return (lib_sort_t)i;
    return dflt;
}

/* The Sort row's index of a sort, under the scope's table. */
static int sort_row_of(lib_sort_t k)
{
    for(int i = 0; i < sort_rows; i++) if(sort_map[i] == k) return i;
    return 0;
}

static const char * opens_name(int o) { return o == 1 ? "tracks" : o == 2 ? "tracks-album" : "albums"; }

static void read_options(void)
{
    char key[64], v[24];
    view_opts.style = 0;
    view_opts.style_count = 1;
    view_opts.styles[0] = T(S_STYLE_GRID);
    if(scope == GRID_SCOPE_TRACKS || scope == GRID_SCOPE_FOLDER) {
        /* the view's other presentations live in listpage.c */
        if(ui_screen.kind != UI_SCREEN_TV) view_opts.styles[view_opts.style_count++] = T(S_STYLE_TILES);
        view_opts.styles[view_opts.style_count++] = T(S_STYLE_LIST);
    }
    else if(scope == GRID_SCOPE_ARTISTS) {
        /* Rows (rows.c), this grid, the list page: the grid is the second */
        view_opts.style_count = 0;
        view_opts.styles[view_opts.style_count++] = T(S_STYLE_ROWS);
        view_opts.styles[view_opts.style_count++] = T(S_STYLE_GRID);
        view_opts.styles[view_opts.style_count++] = T(S_STYLE_LIST);
        view_opts.style = 1;
    }
    /* the name line over the tiles (0.19a, Ian: not wanted over a folder's
     * covers; a switch for the views that have it) */
    view_key("names", key, sizeof(key));
    view_opts.names_row = true;
    view_opts.names = config_read_bool(key, scope == GRID_SCOPE_ALBUMS || scope == GRID_SCOPE_ARTISTS);
    view_opts.size = read_size();
    view_opts.sort_name = NULL;
    /* the Sort row: the scope's table of sorts */
    sort_rows = 0;
    switch(scope) {
        case GRID_SCOPE_FOLDER:
            /* a folder's children in the folder's own order (by name), or the
             * newest first (0.19a) */
            sort_map[sort_rows] = LIB_SORT_TITLE;  view_opts.sorts[sort_rows++] = T(S_SORT_NAME);
            sort_map[sort_rows] = LIB_SORT_NEWEST; view_opts.sorts[sort_rows++] = T(S_SORT_NEWEST);
            break;
        case GRID_SCOPE_ARTIST:
            /* one artist's albums: in year order, by title, or the newest on the card */
            sort_map[sort_rows] = LIB_SORT_YEAR;   view_opts.sorts[sort_rows++] = T(S_SORT_YEAR);
            sort_map[sort_rows] = LIB_SORT_TITLE;  view_opts.sorts[sort_rows++] = T(S_SORT_TITLE);
            sort_map[sort_rows] = LIB_SORT_NEWEST; view_opts.sorts[sort_rows++] = T(S_SORT_NEWEST);
            break;
        case GRID_SCOPE_ARTISTS:
            /* artists are by name; the row says what an artist opens as */
            sort_map[sort_rows++] = LIB_SORT_TITLE;
            break;
        case GRID_SCOPE_COLLECTION:
            /* a collection's albums as they were added, or by title, artist, year */
            sort_map[sort_rows] = SORT_AS_ADDED;         view_opts.sorts[sort_rows++] = T(S_SORT_ADDED);
            sort_map[sort_rows] = LIB_SORT_TITLE;        view_opts.sorts[sort_rows++] = T(S_SORT_TITLE);
            sort_map[sort_rows] = LIB_SORT_ARTIST_TITLE; view_opts.sorts[sort_rows++] = T(S_SORT_ARTIST_TITLE);
            sort_map[sort_rows] = LIB_SORT_YEAR;         view_opts.sorts[sort_rows++] = T(S_SORT_YEAR);
            break;
        default:
            sort_map[sort_rows] = LIB_SORT_TITLE;        view_opts.sorts[sort_rows++] = T(S_SORT_TITLE);
            sort_map[sort_rows] = LIB_SORT_ARTIST_TITLE; view_opts.sorts[sort_rows++] = T(S_SORT_ARTIST_TITLE);
            sort_map[sort_rows] = LIB_SORT_ARTIST_YEAR;  view_opts.sorts[sort_rows++] = T(scope == GRID_SCOPE_TRACKS ? S_SORT_ARTIST_ALBUM : S_SORT_ARTIST_YEAR);
            sort_map[sort_rows] = LIB_SORT_YEAR;         view_opts.sorts[sort_rows++] = T(S_SORT_YEAR);
            sort_map[sort_rows] = LIB_SORT_NEWEST;       view_opts.sorts[sort_rows++] = T(S_SORT_NEWEST);
            break;
    }
    sort_kind = read_sort(sort_map[0]);
    if(sort_row_of(sort_kind) == 0 && sort_kind != sort_map[0]) sort_kind = sort_map[0];   /* a sort this scope doesn't offer */
    view_opts.sort_count = sort_rows;
    view_opts.sort = sort_row_of(sort_kind);
    if(scope == GRID_SCOPE_ARTISTS) {
        view_opts.sort_name = T(S_PICKER_OPENS);
        view_opts.sort_count = 3;
        view_opts.sorts[0] = T(S_OPENS_ALBUMS);
        view_opts.sorts[1] = T(S_OPENS_TRACKS);
        view_opts.sorts[2] = T(S_OPENS_TRACKS_ALBUM);
        view_key("opens", key, sizeof(key));
        config_read(key, v, sizeof(v), "albums");
        opens = strcmp(v, "tracks") == 0 ? 1 : strcmp(v, "tracks-album") == 0 ? 2 : 0;
        view_opts.sort = opens;
    }
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
    config_write(key, sort_kind == SORT_AS_ADDED ? "added" : library_sort_name(sort_kind));
    view_key("jump", key, sizeof(key));
    config_write(key, view_opts.strip ? "1" : "0");
    view_key("names", key, sizeof(key));
    config_write(key, view_opts.names ? "1" : "0");
    if(scope == GRID_SCOPE_TRACKS || scope == GRID_SCOPE_FOLDER) {
        view_key("style", key, sizeof(key));
        config_write(key, view_opts.style == 0 ? "grid" : (ui_screen.kind != UI_SCREEN_TV && view_opts.style == 1) ? "tiles" : "list");
    }
    else if(scope == GRID_SCOPE_ARTISTS) {
        view_key("style", key, sizeof(key));
        config_write(key, view_opts.style == 0 ? "rows" : view_opts.style == 2 ? "list" : "grid");
        view_key("opens", key, sizeof(key));
        config_write(key, opens_name(opens));
    }
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
    if(t->t_keyline) {
        /* the frame grows with the tile (0.19a: it had stayed at the resting size) */
        int in = side * 5 / 100, bw = side / 70 < 2 ? 2 : side / 70;
        lv_obj_set_pos(t->t_keyline, in, in);
        lv_obj_set_size(t->t_keyline, side - 2 * in, side - 2 * in);
        lv_obj_set_style_border_width(t->t_keyline, bw, 0);
    }
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
    if(!entry_has_art(t->entry)) return;
    int want = (t->entry == sel && !ring_mode()) ? g.tile_focus : g.tile;
    bmp_t * b = bmp_find(t->entry, want);
    if(!b) b = bmp_find(t->entry, t->entry == sel ? g.tile : g.tile_focus);
    if(b) { if(t->shown_px != b->px || lv_image_get_src(t->img) != &b->dsc) tile_apply_bitmap(t, b); }
}

/* The no-art text: tiletext.c (0.16b rules, moved out in 0.17 for the album
 * page). The dealt style gives the colours and cover font. */

static tile_style_t * styles;         /* per entry: colours + cover font, dealt per visit */
static int            styles_n;

static void tile_make_text(tile_t * t)
{
    const tile_style_t * st = (t->entry >= 0 && t->entry < styles_n) ? &styles[t->entry] : NULL;
    tiletext_t tt;
    tiletext_make(t->obj, g.tile, entry_title(t->entry), entry_artist(t->entry), st, &tt);
    t->t_box = tt.box;
    t->t_artist = tt.artist;
    t->t_title = tt.title;
    t->t_extra = tt.extra;
    if(entries[t->entry].kind == ENTRY_FOLDER && st && mixed_kinds) {
        /* a folder among albums: a keyline inside the tile says "not a
         * record" (0.19); a grid of artist tiles alone needs none */
        lv_obj_t * k = lv_obj_create(t->obj);
        lv_obj_remove_style_all(k);
        lv_obj_add_flag(k, LV_OBJ_FLAG_IGNORE_LAYOUT);
        int in = g.tile * 5 / 100, bw = g.tile / 70 < 2 ? 2 : g.tile / 70;
        lv_obj_set_pos(k, in, in);
        lv_obj_set_size(k, g.tile - 2 * in, g.tile - 2 * in);
        lv_obj_set_style_border_color(k, lv_color_hex(st->ink), 0);
        lv_obj_set_style_border_width(k, bw, 0);
        lv_obj_set_style_border_opa(k, LV_OPA_COVER, 0);
        lv_obj_move_background(k);
        t->t_keyline = k;
    }
}

/* The no-art colours (0.16f, Ian 9 Oct: more variety). Dealt per visit by
 * tilestyle.c from the theme's tilecolours setting - Ian's colour sets, a
 * mix of them, or colourways generated from them - so a screenful uses each
 * colour once before repeating and no tile matches the one left or above.
 * Held while in the grid: the same seed, entries and columns deal the same
 * colours again, so paging back shows what it showed before. */
static void deal_colours(void)
{
    free(styles);
    styles = NULL;
    styles_n = 0;
    if(nentries == 0) return;
    styles = calloc((size_t)nentries, sizeof(*styles));
    bool * noart = calloc((size_t)nentries, sizeof(bool));
    int * letters = calloc((size_t)nentries, sizeof(int));
    if(styles && noart && letters) {
        for(int e = 0; e < nentries; e++) {
            noart[e] = !entry_has_art(e);
            char m[300], b[300];
            const char * title = entry_title(e);
            letters[e] = count_letters(split_bracket(title, m, sizeof(m), b, sizeof(b)) ? m : title);
        }
        tilestyle_deal(visit_seed, nentries, cols(), noart, letters, styles);
        styles_n = nentries;
    }
    free(noart);
    free(letters);
}

static uint32_t tile_colour(int entry, uint32_t * ink)
{
    const lib_album_t * a = album_of(entry);
    if(a && a->has_art && a->colour) { if(ink) *ink = 0xFFFFFF; return a->colour; }
    if((entries[entry].kind == ENTRY_FOLDER || entries[entry].kind == ENTRY_ARTIST) && entries[entry].pic) { if(ink) *ink = 0xFFFFFF; return 0x333333; }
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

    if(!entry_has_art(entry)) tile_make_text(t);
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

/* The spare cells of a small folder or artist (0.20a, Ian 14 Oct: the
 * coloured spare cells of the artists' mosaic "make me reconsider the
 * album folder variable grid"): when the whole scope fits on one screen,
 * the cells after the last tile are flat squares in dealt colours. */
#define MAX_FILLERS 64
static lv_obj_t * fillers[MAX_FILLERS];
static int        nfillers;

static void fillers_drop(void)
{
    for(int i = 0; i < nfillers; i++) if(fillers[i]) lv_obj_delete(fillers[i]);
    nfillers = 0;
}

static void fillers_make(void)
{
    fillers_drop();
    if((scope != GRID_SCOPE_FOLDER && scope != GRID_SCOPE_ARTIST && scope != GRID_SCOPE_COLLECTION) || nentries == 0 || rows_total > rows_on_screen()) return;
    int cells = rows_on_screen() * cols();
    if(cells <= nentries || cells > MAX_FILLERS) return;
    tile_style_t * st = calloc((size_t)cells, sizeof(*st));
    bool * noart = calloc((size_t)cells, sizeof(bool));
    int * letters = calloc((size_t)cells, sizeof(int));
    if(st && noart && letters) {
        for(int i = 0; i < cells; i++) noart[i] = true;
        tilestyle_deal(visit_seed ^ 0x5A17C0DEu, cells, cols(), noart, letters, st);
        int inset = (g.cell - g.tile) / 2;
        for(int e = nentries; e < cells; e++) {
            lv_obj_t * o = lv_obj_create(field);
            lv_obj_remove_style_all(o);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(o, col_of(e) * g.cell + inset, row_of(e) * g.cell + inset);
            lv_obj_set_size(o, g.tile, g.tile);
            /* a filler's colour never matches the tile before it */
            uint32_t c = st[e].bg;
            if(e - 1 < styles_n && e - 1 >= 0 && styles[e - 1].bg == c) c = st[e].ink;
            lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_move_background(o);
            fillers[nfillers++] = o;
        }
    }
    free(st); free(noart); free(letters);
}

static void tiles_clear(void)
{
    for(int i = 0; i < MAX_TILES; i++) if(tiles[i].entry >= 0) tile_delete(&tiles[i]);
    fillers_drop();
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
    if(ts) focus_tile(ts, picker_mode() != PICKER_STRIP, false);
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
            if(!entry_has_art(e)) continue;
            const lib_album_t * al = album_of(e);
            int px = (k == 0 && !ring_mode()) ? g.tile_focus : g.tile;
            if(bmp_find(e, px)) continue;
            bool dup = false;
            for(int i = 0; i < n; i++) if(reqs[i].id == e && reqs[i].px == px) { dup = true; break; }
            if(dup) continue;
            if(al) {
                int si = thumb_index_for(px);
                if(!(al->thumb_sizes & (1 << si))) {
                    /* that size not cached: the biggest that is */
                    int found = -1;
                    for(int i = LIB_THUMB_COUNT - 1; i >= 0; i--) if(al->thumb_sizes & (1 << i)) { found = i; break; }
                    if(found < 0) continue;
                    si = found;
                }
                cache_thumb_path(card_of(e), al, si, reqs[n].path, sizeof(reqs[n].path));
            }
            else snprintf(reqs[n].path, sizeof(reqs[n].path), "%s", entries[e].pic);   /* a folder's own picture, fitted by the decoder */
            reqs[n].id = e;
            reqs[n].gen = list_gen;
            reqs[n].px = px;
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
    power_indicator_yield(false);
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
static void name_fill(nlabel_t * main, nlabel_t * tail, int box_w, int px, int e)
{
    char text[700], mainpart[300], br[300];
    const char * ar = entry_artist(e), * title = entry_title(e), * release = entry_release(e);
    bool has_br = split_bracket(title, mainpart, sizeof(mainpart), br, sizeof(br));
    if(ar[0]) snprintf(text, sizeof(text), "%s \xE2\x80\x93 %s", ar, has_br ? mainpart : title);
    else snprintf(text, sizeof(text), "%s", has_br ? mainpart : title);
    /* a release of an album that exists twice: its label joins the tail
     * (11 Oct) - "2019 Remaster", or "(Deluxe) \xc2\xb7 2019 Remaster" */
    if(release[0]) {
        if(has_br) { size_t o = strlen(br); snprintf(br + o, sizeof(br) - o, " \xC2\xB7 %s", release); }
        else snprintf(br, sizeof(br), "%s", release);
        has_br = true;
    }
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
    if(!view_opts.names) {
        if(g.band_h) { lv_label_set_text(band_main.l[0], ""); lv_label_set_text(band_tail.l[0], ""); }
        else lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if(g.band_h) { name_fill(&band_main, &band_tail, g.w - 2 * mm.margin, mm.font_px, sel); return; }
    if(picker_mode() == PICKER_STRIP) return;              /* the pill is driving: no subtitle under it */
    name_fill(&name_main, &name_tail, g.w - 2 * mm.margin, mm.font_px, sel);
    lv_obj_remove_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(namebar);
    power_indicator_yield(true);                            /* the battery disc steps aside (0.18b) */
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
    /* under the pill no cover is focused (Ian, 10 Oct): a focused cover
     * says Left/Right would move it, when they move the letter */
    if(tn) focus_tile(tn, !pill, old != e && !pill);
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

/* An entry's year and "added" date (a track's from its album / itself). */
static int entry_year(int e)
{
    const lib_album_t * a = album_of(e);
    return a ? a->year : 0;
}

static long entry_newest(int e)
{
    const lib_track_t * t = track_of(e);
    if(t) return t->added ? t->added : t->mtime;
    if(entries[e].kind == ENTRY_FOLDER || entries[e].kind == ENTRY_ARTIST) return entries[e].newest;
    const lib_album_t * a = album_of(e);
    return a ? a->newest : 0;
}

/* Whether an entry lacks what the sort needs (goes to the end section). */
static bool entry_lacks(int e, lib_sort_t sort)
{
    if(entries[e].kind == ENTRY_ALBUM) return library_album_lacks(album_of(e), sort);
    if(entries[e].kind == ENTRY_FOLDER || entries[e].kind == ENTRY_ARTIST) return false;
    switch(sort) {
        case LIB_SORT_ARTIST_TITLE: case LIB_SORT_ARTIST_YEAR: return entry_artist(e)[0] == 0;
        case LIB_SORT_YEAR: return entry_year(e) == 0;
        default: return false;
    }
}

static int entry_cmp(const void * pa, const void * pb)
{
    const entry_t * a = pa, * b = pb;
    int ea = (int)(a - entries), eb = (int)(b - entries);
    int c = 0;
    if(a->kind == ENTRY_ALBUM && b->kind == ENTRY_ALBUM)
        c = library_album_cmp(album_of(ea), album_of(eb), sort_kind);
    else {
        /* tracks (and the odd folder): the end section by folder path, else by the sort's keys */
        bool la = entry_lacks(ea, sort_kind), lb = entry_lacks(eb, sort_kind);
        if(la != lb) return la ? 1 : -1;
        if(la) c = strcmp(track_of(ea) ? track_of(ea)->uri : entries[ea].uri, track_of(eb) ? track_of(eb)->uri : entries[eb].uri);
        else switch(sort_kind) {
            case LIB_SORT_ARTIST_TITLE:
                c = library_name_cmp(entry_artist(ea), entry_artist(eb));
                if(!c) c = library_name_cmp(entry_title(ea), entry_title(eb));
                break;
            case LIB_SORT_ARTIST_YEAR: {        /* tracks: artist, album, track number */
                c = library_name_cmp(entry_artist(ea), entry_artist(eb));
                const lib_album_t * x = album_of(ea), * y = album_of(eb);
                if(!c && x && y) c = library_name_cmp(x->title, y->title);
                if(!c && track_of(ea) && track_of(eb)) {
                    const lib_track_t * s = track_of(ea), * t = track_of(eb);
                    c = s->disc != t->disc ? (s->disc < t->disc ? -1 : 1) : s->track != t->track ? (s->track < t->track ? -1 : 1) : 0;
                }
                break;
            }
            case LIB_SORT_YEAR:
                c = entry_year(ea) - entry_year(eb);
                if(!c) c = library_name_cmp(entry_artist(ea), entry_artist(eb));
                if(!c) c = library_name_cmp(entry_title(ea), entry_title(eb));
                break;
            case LIB_SORT_NEWEST:
                c = entry_newest(ea) > entry_newest(eb) ? -1 : entry_newest(ea) < entry_newest(eb) ? 1 : 0;
                break;
            default:
                c = library_name_cmp(entry_title(ea), entry_title(eb));
                if(!c) c = library_name_cmp(entry_artist(ea), entry_artist(eb));
                break;
        }
    }
    if(!c) c = a->src - b->src;
    if(!c) c = a->kind - b->kind;
    if(!c) c = a->album - b->album;
    if(!c) c = a->track - b->track;
    return c;
}

/* An entry outside the alphabet: the Various Artists tile, which sits last. */
static bool entry_unlettered(int e)
{
    if(entries[e].kind != ENTRY_ARTIST) return false;
    const artist_t * a = artists_get(entries[e].artist);
    return a && a->various;
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
    if(sort_kind == LIB_SORT_NEWEST || sort_kind == SORT_AS_ADDED) { for(int e = 0; e < nentries; e++) entries[e].label = -1; return; }

    bool end_section = false;
    for(int e = 0; e < nentries; e++) if(entry_lacks(e, sort_kind)) { end_section = true; break; }

    if(by_year && sort_kind == LIB_SORT_YEAR) {
        /* decades, in order of appearance (the list is sorted by year) */
        for(int e = 0; e < nentries; e++) {
            if(entry_lacks(e, sort_kind)) { entries[e].label = -1; continue; }
            char d[PICKER_LABEL_LEN];
            snprintf(d, sizeof(d), "%d0s", (entry_year(e) / 10) % 1000);
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
            if(entry_lacks(e, sort_kind) || entry_unlettered(e)) continue;
            names[n++] = by_artist ? entry_artist(e) : entry_title(e);
        }
        picker_labels_from_names(&view_opts, names, n, end_section);
        /* the pill steps through the letters in the order the list shows
         * them (the list is in byte order until the collation pass; the
         * picker groups by script), so reorder by first appearance */
        static char ordered[PICKER_MAX_LABELS][PICKER_LABEL_LEN];
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
        for(int e = 0; e < nentries; e++)
            entries[e].label = (entry_lacks(e, sort_kind) || entry_unlettered(e)) ? -1 : picker_label_of(&view_opts, by_artist ? entry_artist(e) : entry_title(e));
    }
    if(end_section) {
        /* the "?" chip is the last label */
        int q = view_opts.label_count - 1;
        if(q >= 0 && strcmp(view_opts.labels[q], "?") == 0)
            for(int e = 0; e < nentries; e++) if(entry_lacks(e, sort_kind)) entries[e].label = q;
    }
    view_opts.jump = view_opts.label_count ? 0 : -1;
}

static void sort_entries(void)
{
    if(nentries > 1 && scope != GRID_SCOPE_FOLDER && scope != GRID_SCOPE_ARTISTS && sort_kind != SORT_AS_ADDED) qsort(entries, (size_t)nentries, sizeof(entry_t), entry_cmp);
    else if(nentries > 1 && sort_kind == LIB_SORT_NEWEST) {
        /* a folder's children newest first (0.19a); the index numbers keep the
         * folder's own order for ties, so sort a copy of the order */
        int * order = malloc(sizeof(int) * (size_t)nentries);
        entry_t * copy = malloc(sizeof(entry_t) * (size_t)nentries);
        if(order && copy) {
            for(int i = 0; i < nentries; i++) order[i] = i;
            /* insertion sort by newest, stable, n is small (one folder) */
            for(int i = 1; i < nentries; i++) {
                int k = order[i], j = i - 1;
                while(j >= 0 && entry_newest(order[j]) < entry_newest(k)) { order[j + 1] = order[j]; j--; }
                order[j + 1] = k;
            }
            for(int i = 0; i < nentries; i++) copy[i] = entries[order[i]];
            memcpy(entries, copy, sizeof(entry_t) * (size_t)nentries);
        }
        free(order);
        free(copy);
    }
    build_labels();
}

static void entries_add(int src, entry_kind_t kind, int album, int track, const char * name, const char * uri, const char * pic)
{
    entry_t * e = &entries[nentries++];
    memset(e, 0, sizeof(*e));
    e->src = src;
    e->kind = kind;
    e->album = album;
    e->track = track;
    e->label = -1;
    e->artist = -1;
    if(name) e->name = strdup(name);
    if(uri) e->uri = strdup(uri);
    if(pic) e->pic = strdup(pic);
}

/* A folder's direct music subfolders as tiles (0.19, Note 05 section 7):
 * an album folder is its album's cover; any other gets the folder's own
 * picture (artist.jpg / folder.jpg / the only image in it) or a name tile. */
static void entries_from_folder(int src, const char * folder)
{
    const library_t * lib = &libs_get(src)->lib;
    size_t fl = strlen(folder);
    for(int k = 0; k < lib->nfolders; k++) {
        const lib_folder_t * f = &lib->folders[k];
        if(fl) { if(strncmp(f->uri, folder, fl) != 0 || f->uri[fl] != '/') continue; }
        else if(!f->uri[0]) continue;
        const char * rest = f->uri + (fl ? fl + 1 : 0);
        if(strchr(rest, '/')) continue;                       /* deeper than one level */
        if(f->tracks == 0 && f->subfolders == 0) continue;    /* no music in it */
        if(mpd_is_container_name(f->uri)) continue;          /* a disc image is part of its folder */
        if(f->type == FOLDER_ALBUM && f->album >= 0 && f->album < lib->nalbums) { entries_add(src, ENTRY_ALBUM, f->album, -1, NULL, NULL, NULL); continue; }
        char pic[1024];
        bool has_pic = art_folder_image(f->uri, pic, sizeof(pic));
        entries_add(src, ENTRY_FOLDER, -1, -1, rest, f->uri, has_pic ? pic : NULL);
        /* its newest album, for the Newest sort */
        entry_t * e = &entries[nentries - 1];
        size_t ul = strlen(f->uri);
        for(int a = 0; a < lib->nalbums; a++) {
            const lib_album_t * al = &lib->albums[a];
            if(strncmp(al->folder, f->uri, ul) != 0 || (al->folder[ul] != '/' && al->folder[ul] != 0)) continue;
            if(al->newest > e->newest) e->newest = al->newest;
        }
    }
}

/* Build the entries for the scope from the loaded indexes. Puts the cursor
 * on `keep` (an album folder, a folder URI or a track URI) when it is there. */
static void load_library(const char * keep)
{
    for(int e = 0; e < nentries; e++) { free(entries[e].name); free(entries[e].uri); free(entries[e].pic); }
    entries_free();
    lib_gen = libs_generation();

    int total = 0;
    for(int s = 0; s < libs_count(); s++) total += scope == GRID_SCOPE_TRACKS ? libs_get(s)->lib.ntracks : libs_get(s)->lib.nalbums + libs_get(s)->lib.nfolders;
    bool by_artist = scope == GRID_SCOPE_ARTISTS || scope == GRID_SCOPE_ARTIST;
    if(by_artist) {
        /* the artists of every card, merged (artists.h); or one artist's albums */
        artists_refresh();
        int one = scope == GRID_SCOPE_ARTIST ? artists_find(scope_folder) : -1;
        total = scope == GRID_SCOPE_ARTISTS ? artists_count() : one >= 0 ? artists_get(one)->nalbums : 0;
    }
    cur_gen = curation_generation();
    const cur_list_t * coll = scope == GRID_SCOPE_COLLECTION ? curation_by_uid(curation_find_key(scope_folder)) : NULL;
    if(scope == GRID_SCOPE_COLLECTION) total = coll ? coll->n : 0;
    entries = total ? calloc((size_t)total, sizeof(entry_t)) : NULL;
    if(coll && entries) {
        /* the albums the collection names that are on a card: the folder,
         * and the title where two releases share it (0.21) */
        for(int k = 0; k < coll->n; k++) {
            int bs = -1, ba = -1;
            for(int s = 0; s < libs_count() && ba < 0; s++) {
                const library_t * lib = &libs_get(s)->lib;
                if(strncmp(coll->uris[k], libs_get(s)->card, strlen(libs_get(s)->card)) != 0) continue;
                for(int a = 0; a < lib->nalbums; a++) {
                    if(strcmp(lib->albums[a].folder, coll->uris[k]) != 0) continue;
                    if(bs < 0) { bs = s; ba = a; }
                    if(coll->titles[k][0] && strcmp(lib->albums[a].title, coll->titles[k]) == 0) { bs = s; ba = a; break; }
                }
            }
            if(ba >= 0) entries_add(bs, ENTRY_ALBUM, ba, -1, NULL, NULL, NULL);
        }
    }
    else if(by_artist) {
        int one = scope == GRID_SCOPE_ARTIST ? artists_find(scope_folder) : -1;
        for(int i = 0; i < artists_count() && entries; i++) {
            const artist_t * a = artists_get(i);
            if(scope == GRID_SCOPE_ARTISTS) {
                entries_add(a->nalbums ? a->albums[0].src : 0, ENTRY_ARTIST, -1, -1, a->name, NULL, a->pic);
                entries[nentries - 1].artist = i;
                for(int k = 0; k < a->nalbums; k++) { long nw = libs_get(a->albums[k].src)->lib.albums[a->albums[k].album].newest; if(nw > entries[nentries - 1].newest) entries[nentries - 1].newest = nw; }
            }
            else if(i == one) for(int k = 0; k < a->nalbums; k++) entries_add(a->albums[k].src, ENTRY_ALBUM, a->albums[k].album, -1, NULL, NULL, NULL);
        }
    }
    for(int s = 0; s < libs_count() && entries && !by_artist && scope != GRID_SCOPE_COLLECTION; s++) {
        const library_t * lib = &libs_get(s)->lib;
        if(scope == GRID_SCOPE_ALBUMS) {
            for(int a = 0; a < lib->nalbums; a++) {
                if(lib->albums[a].in_collection) continue;        /* singles in a collection folder */
                entries_add(s, ENTRY_ALBUM, a, -1, NULL, NULL, NULL);
            }
        }
        else if(scope == GRID_SCOPE_TRACKS) {
            for(int t = 0; t < lib->ntracks; t++) entries_add(s, ENTRY_TRACK, lib->tracks[t].album, t, NULL, NULL, NULL);
        }
        else {
            /* the folder is one card's */
            if(strncmp(scope_folder, libs_get(s)->card, strlen(libs_get(s)->card)) != 0) continue;
            entries_from_folder(s, scope_folder);
        }
    }
    sort_entries();
    mixed_kinds = false;
    { bool alb = false, fol = false; for(int e = 0; e < nentries; e++) { if(entries[e].kind == ENTRY_ALBUM) alb = true; if(entries[e].kind == ENTRY_FOLDER) fol = true; } mixed_kinds = alb && fol; }
    rows_total = (nentries + cols() - 1) / cols();
    sel = 0;
    if(keep) { int e = find_key(keep); if(e >= 0) sel = e; }
    fprintf(stderr, "simpleton-ui: grid: %s%s%s: %d tiles from %d card%s, %s, %d labels\n",
            view_id(), scope == GRID_SCOPE_FOLDER || scope == GRID_SCOPE_ARTIST ? " " : "", scope == GRID_SCOPE_FOLDER || scope == GRID_SCOPE_ARTIST ? scope_folder : "",
            nentries, libs_count(), libs_count() == 1 ? "" : "s", sort_kind == SORT_AS_ADDED ? "as added" : library_sort_name(sort_kind), view_opts.label_count);
}

static const char * entry_key(int e)
{
    return entries[e].kind == ENTRY_FOLDER ? entries[e].uri : entries[e].kind == ENTRY_ARTIST ? entries[e].name : entries[e].kind == ENTRY_TRACK ? track_of(e)->uri : album_of(e)->folder;
}

static int find_key(const char * key)
{
    for(int e = 0; e < nentries; e++) { const char * k = entry_key(e); if(k && strcmp(k, key) == 0) return e; }
    return -1;
}

/* What the cursor is on, as a key load_library() can find again. */
static char * keep_key(void)
{
    if(nentries == 0 || sel < 0 || sel >= nentries) return NULL;
    const char * k = entry_key(sel);
    return k ? strdup(k) : NULL;
}

/* ------------------------------------------------------------ layout */

/* A folder with few albums fills the screen with them (Ian, 13 Oct: one
 * album the whole screen, two to four as 2 x 2, five to nine as 3 x 3), and
 * so does one artist's handful (0.20); a bigger folder, and every other
 * scope, follows the picker's size. */
static ui_size_t fit_size(void)
{
    if((scope != GRID_SCOPE_FOLDER && scope != GRID_SCOPE_ARTIST && scope != GRID_SCOPE_COLLECTION) || nentries == 0) return view_opts.size;
    for(ui_size_t s = UI_SIZE_XXL; s <= UI_SIZE_L; s++) {
        ui_grid_t t;
        ui_grid_metrics(s, &t);
        if(t.rows * t.cols >= nentries) return s;
    }
    return view_opts.size;
}

static void measure(void)
{
    ui_grid_metrics(fit_size(), &g);
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
    /* back from an album (Ian, 17 Oct: "I end up back in the middle row"):
     * the cursor's row stays where it was on screen; a fresh grid, or one
     * whose rows changed, centres it */
    bool visible = row_of(sel) >= first_row && row_of(sel) < first_row + rows_on_screen();
    if(!(returning && visible)) first_row = row_of(sel) - rows_on_screen() / 2;
    returning = false;
    if(first_row > rows_total - rows_on_screen()) first_row = rows_total - rows_on_screen();
    if(first_row < 0) first_row = 0;
    lv_obj_scroll_to_y(field, first_row * g.cell, LV_ANIM_OFF);
    if(nentries == 0) {
        lv_label_set_text(msg, T(libs_loaded() ? (scope == GRID_SCOPE_TRACKS ? S_LIST_EMPTY : scope == GRID_SCOPE_ARTISTS ? S_ARTISTS_EMPTY : scope == GRID_SCOPE_COLLECTION ? S_LIST_COLLECTION_EMPTY : S_GRID_EMPTY) : S_BROWSER_STARTING));
        lv_obj_remove_flag(msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
        power_indicator_yield(false);
        if(g.band_h) { lv_label_set_text(band_main.l[0], ""); lv_label_set_text(band_tail.l[0], ""); }
        return;
    }
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    window_normal();
    fillers_make();
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
    bool resort = scope != GRID_SCOPE_ARTISTS && o->sort >= 0 && o->sort < sort_rows && sort_map[o->sort] != sort_kind;
    bool restyle = o->style != view_opts.style;
    bool renames = o->names != view_opts.names;
    view_opts.size = o->size;
    view_opts.strip = o->strip;
    view_opts.style = o->style;
    view_opts.names = o->names;
    if(scope == GRID_SCOPE_ARTISTS) { if(opens != o->sort) renames = true; opens = o->sort; view_opts.sort = o->sort; }   /* the band's count follows "Opens as" */
    if(restyle && (scope == GRID_SCOPE_TRACKS || scope == GRID_SCOPE_FOLDER) && view_opts.style != 0) { write_options(); if(style_cb) style_cb("list"); return; }
    if(restyle && scope == GRID_SCOPE_ARTISTS && view_opts.style != 1) { write_options(); if(style_cb) style_cb(view_opts.style == 0 ? "rows" : "list"); return; }
    if(renames) show_name();
    if(resort) {
        sort_kind = sort_map[o->sort];
        view_opts.sort = o->sort;
        char * keep = keep_key();
        sort_entries();
        if(keep) { int e = find_key(keep); if(e >= 0) sel = e; free(keep); }
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
    tile_t * t = tile_of(sel);
    if(t) focus_tile(t, false, false);                 /* the pill has the D-pad: no cover comes forward */
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
    if(libs_refresh() || lib_gen != libs_generation() || (scope == GRID_SCOPE_COLLECTION && cur_gen != curation_generation())) {
        char * keep = keep_key();
        load_library(keep);
        free(keep);
        rebuild();
    }
}

void grid_create(void (*on_open)(const library_t *, int, const char *, const tile_style_t *), void (*on_exit)(void))
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
    static unsigned seeds[GRID_SCOPE_COLLECTION + 1]; /* each scope's last deal: coming back from a deeper view wears the same colours (0.20) */
    shown = true;
    const grid_memory_t * mem = fresh ? NULL : memory_find();
    if(fresh) visit_seed = (unsigned)time(NULL) ^ (unsigned)lv_tick_get() ^ (unsigned)rand();
    else if(mem) visit_seed = mem->seed;
    else if(seeds[scope]) visit_seed = seeds[scope];
    seeds[scope] = visit_seed;
    suppress_overlay = !fresh;                 /* back from an album: no pill, the cursor on it */
    returning = !fresh;
    read_options();
    lv_screen_load(scr);
    picker_attach(scr, &plm);
    lv_timer_resume(poll_timer);
    lv_timer_resume(index_timer);
    if(libs_refresh() || lib_gen != libs_generation() || scope_changed || (scope == GRID_SCOPE_COLLECTION && cur_gen != curation_generation())) {
        char * keep = scope_changed ? NULL : keep_key();
        load_library(keep);
        free(keep);
        scope_changed = false;
    }
    if(mem && mem->sel >= 0 && mem->sel < nentries && (sel == 0 || sel == mem->sel)) { sel = mem->sel; first_row = mem->first_row; }   /* a reloaded grid: the cursor and the rows as they were */
    rebuild();
    after_enter();
}

static void memory_store(void)
{
    int at = 7;
    for(int i = 0; i < 8; i++) if(memories[i].used && memories[i].scope == scope && strcmp(memories[i].folder, scope_folder) == 0) { at = i; break; }
    for(int i = at; i > 0; i--) memories[i] = memories[i - 1];
    memories[0].used = true;
    memories[0].scope = scope;
    snprintf(memories[0].folder, sizeof(memories[0].folder), "%s", scope_folder);
    memories[0].seed = visit_seed;
    memories[0].sel = sel;
    memories[0].first_row = first_row;
}

void grid_hide(void)
{
    shown = false;
    if(!scope_changed) memory_store();              /* (a scope already set for the next show was stored then) */
    if(namebar_timer) { lv_timer_delete(namebar_timer); namebar_timer = NULL; }
    lv_obj_add_flag(namebar, LV_OBJ_FLAG_HIDDEN);
    power_indicator_yield(false);
    if(picker_mode() != PICKER_NONE) picker_close();
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
    thumbs_want(NULL, 0);
    tiles_clear();
    bmp_clear_all();
}

bool grid_overlay_active(void) { return shown && picker_mode() != PICKER_NONE; }

bool grid_lookup_folder(const char * uri, const library_t ** lib, int * album, const char ** card, const tile_style_t ** tile)
{
    if(!uri || !uri[0]) return false;
    libs_refresh();
    const lib_folder_t * f;
    const lib_source_t * src = libs_find_folder(uri, &f);
    if(!src || f->type != FOLDER_ALBUM || f->album < 0 || f->album >= src->lib.nalbums) return false;
    *lib = &src->lib;
    *album = f->album;
    *card = src->card;
    *tile = NULL;
    if(!src->lib.albums[f->album].has_art && lib_gen == libs_generation())
        for(int e = 0; e < nentries && e < styles_n; e++)
            if(libs_get(entries[e].src) == src && entries[e].kind == ENTRY_ALBUM && entries[e].album == f->album) { *tile = &styles[e]; break; }
    return true;
}

bool grid_open_folder(const char * folder)
{
    if(!folder || !folder[0] || !open_cb) return false;
    for(int e = 0; e < nentries; e++) {
        if(entries[e].kind != ENTRY_ALBUM || strcmp(album_of(e)->folder, folder) != 0) continue;
        if(shown) set_sel(e, false);
        else sel = e;
        const tile_style_t * st = (!album_of(e)->has_art && e < styles_n) ? &styles[e] : NULL;
        open_cb(lib_of(e), entries[e].album, card_of(e), st);
        return true;
    }
    return false;
}

/* Select on a tile: an album opens (the page), a folder opens its own
 * view, a track plays - the list as shown from here on, through
 * playthrough's list mode (0.19). */
static void select_entry(int e)
{
    if(e < 0 || e >= nentries) return;
    switch(entries[e].kind) {
        case ENTRY_ALBUM:
            if(open_cb) {
                const tile_style_t * st = (!entry_has_art(e) && e < styles_n) ? &styles[e] : NULL;
                open_cb(lib_of(e), entries[e].album, card_of(e), st);
            }
            break;
        case ENTRY_FOLDER:
            if(open_folder_cb) open_folder_cb(entries[e].uri);
            break;
        case ENTRY_ARTIST:
            if(open_artist_cb) open_artist_cb(entries[e].artist);
            break;
        case ENTRY_TRACK: {
            const char ** uris = calloc((size_t)nentries, sizeof(char *));
            if(!uris) return;
            int n = 0;
            placeholder_prefer_tracks_begin();        /* each tile's colours go with its track to now-playing (0.23e) */
            for(int i = 0; i < nentries; i++) if(track_of(i)) { uris[n++] = track_of(i)->uri; if(styles && i < styles_n) tilestyle_prefer_track(track_of(i)->uri, &styles[i], visit_seed); }
            int start = 0;
            for(int i = 0; i < n; i++) if(uris[i] == track_of(e)->uri) { start = i; break; }
            bool started = playthrough_play_list(uris, n, start);
            free(uris);
            if(started && play_cb) play_cb();
            break;
        }
    }
}

/* A held (0.21): the object menu for the tile under the cursor - a track,
 * or an album (and its tracks). */
static void menu_entry(int e)
{
    if(e < 0 || e >= nentries) return;
    if(entries[e].kind == ENTRY_TRACK) { objmenu_open_track(track_of(e)->uri, entry_title(e), entry_artist(e), 0, -1); return; }
    if(entries[e].kind != ENTRY_ALBUM) return;
    const lib_album_t * a = album_of(e);
    const library_t * lib = lib_of(e);
    const char ** uris = calloc((size_t)a->track_count + 1, sizeof(char *));
    int n = 0;
    for(int k = a->first_track; uris && k < lib->ntracks && n < a->track_count; k++) if(lib->tracks[k].album == entries[e].album) uris[n++] = lib->tracks[k].uri;
    const char * coll = scope == GRID_SCOPE_COLLECTION ? scope_folder : NULL;
    int uid = coll ? curation_find_key(coll) : 0, pos = -1;
    if(uid) { const cur_list_t * l = curation_by_uid(uid); for(int k = 0; l && k < l->n && pos < 0; k++) if(strcmp(l->uris[k], a->folder) == 0 && (!l->titles[k][0] || strcmp(l->titles[k], a->title) == 0)) pos = k; }
    objmenu_open_album(a->folder, a->title, a->various ? T(S_VARIOUS_ARTISTS) : a->artist, uris, n, uid, pos);
    free(uris);
}

void grid_curation_changed(void)
{
    if(shown && scope == GRID_SCOPE_COLLECTION && cur_gen != curation_generation()) lv_timer_ready(index_timer);
}

void grid_set_scope(grid_scope_t sc, const char * folder)
{
    const char * f = folder ? folder : "";
    if(sc == scope && strcmp(f, scope_folder) == 0) return;
    if(shown && !scope_changed) memory_store();      /* the grid being left, under its own name */
    scope = sc;
    snprintf(scope_folder, sizeof(scope_folder), "%s", f);
    scope_changed = true;
    read_options();
    measure();
}

grid_scope_t grid_scope(void) { return scope; }
const char * grid_scope_folder(void) { return scope == GRID_SCOPE_FOLDER ? scope_folder : NULL; }
const char * grid_scope_artist(void) { return scope == GRID_SCOPE_ARTIST ? scope_folder : NULL; }
const char * grid_scope_collection(void) { return scope == GRID_SCOPE_COLLECTION ? scope_folder : NULL; }

void grid_set_artist_callback(void (*open_artist)(int artist)) { open_artist_cb = open_artist; }

void grid_set_callbacks(void (*open_folder)(const char * uri), void (*on_play)(void), void (*on_style)(const char * style))
{
    open_folder_cb = open_folder;
    play_cb = on_play;
    style_cb = on_style;
}

bool grid_focus_key(const char * key)
{
    if(!key) return false;
    int e = find_key(key);
    if(e < 0) return false;
    if(shown) set_sel(e, false); else sel = e;
    return true;
}

void grid_toggle_picker(void)
{
    if(!shown) return;
    if(picker_mode() != PICKER_NONE) { picker_toggle_panel(); return; }
    open_full();
}

void grid_show_picker_style(void)
{
    if(!shown || nentries == 0) return;
    if(picker_mode() != PICKER_FULL) { if(picker_mode() != PICKER_NONE) picker_close(); open_full(); }
    picker_focus_style();
}

void grid_reload_options(void)
{
    ui_size_t old_size = view_opts.size;
    lib_sort_t old_sort = sort_kind;
    read_options();
    if(!shown) return;
    if(sort_kind != old_sort) {
        char * keep = keep_key();
        sort_entries();
        if(keep) { int e = find_key(keep); if(e >= 0) sel = e; free(keep); }
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
        case ACT_SELECT:
            select_entry(sel);
            break;
        case ACT_MENU:
            menu_entry(sel);
            break;
        case ACT_BACK:
            if(view_opts.strip && grid_is_long()) open_strip();
            else if(exit_cb) exit_cb();
            break;
        default: break;
    }
}
