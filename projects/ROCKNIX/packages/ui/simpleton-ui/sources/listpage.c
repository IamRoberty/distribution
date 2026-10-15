/*
 * SimpletonOS UI - the list page (implementation). See listpage.h.
 *
 * Shape: a screen of its own holding
 *   square   (TV) the art square at the left or right of the stage
 *   jump     (TV) the "<< R >>" row, a header above the field, pinned
 *   field    the list area, clipping `content`, which is as tall as the
 *            whole list (one row per line of cells) and slides under the
 *            field; only the rows near the screen exist as objects. A row
 *            is one item wide (Tiles, List) or GRID_COLS items wide (the
 *            Tracks grid: text cells, no art).
 * and the shared picker over the field.
 *
 * Measurements: TV rows in 1080ths of the stage (tvpx), handheld rows in
 * stage units (PX), like the album page.
 */
#include "listpage.h"
#include "art.h"
#include "artists.h"
#include "cache.h"
#include "config.h"
#include "history.h"
#include "libs.h"
#include "library.h"
#include "mpdc.h"
#include "picker.h"
#include "placeholder.h"
#include "playthrough.h"
#include "power.h"
#include "strings.h"
#include "textfit.h"
#include "theme.h"
#include "thumbs.h"
#include "tilestyle.h"
#include "tiletext.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define TV_BASE        1080
#define SCROLL_MS      110
#define SETTLE_MS      140       /* the art follows the cursor once it has rested this long */
#define FLASH_MS       180       /* the jump arrow's "boop" */
#define MAX_ROWS       48        /* row objects kept: the screen and the margins, twice over for a slide */
#define GEN_BASE       (1 << 20) /* thumbs ids/gens of ours, apart from the grid's */
#define MOSAIC_BASE    (1 << 28) /* the TV mosaic's cells (0.20)                    */
#define MOSAIC_MAX     25        /* up to 5 x 5 albums in the square                */
#define CHUNK_PAGES    5         /* a jump stop is about this many screenfuls (Ian, 13 Oct) */
#define CHUNK_MAX      4         /* a letter splits into at most this many stops */
#define GROW_PCT       108
#define GRID_COLS      2         /* the Tracks grid: two columns of four text cells (Ian, 13 Oct) */
#define GRID_ROWS      4

/* ---- the items ---- */

typedef enum { ITEM_TRACK, ITEM_ALBUM, ITEM_FOLDER, ITEM_ARTIST, ITEM_HEADER } item_kind_t;   /* ITEM_HEADER (0.20a): an album's title over its tracks, not selectable */

typedef struct {
    int         src;
    item_kind_t kind;
    int         track, album, folder;   /* indexes into the source's arrays; -1 where not that */
    int         artist;                 /* ITEM_ARTIST: the artists.h index; LIST_ARTIST_TRACKS: the track's album's place in the artist's list (0.20) */
    int         section;
    char *      pic;                    /* ITEM_FOLDER / ITEM_ARTIST: an own picture, or NULL */
    int         count;                  /* ITEM_FOLDER / ITEM_ARTIST: albums under it */
    long        newest;                 /* ITEM_FOLDER: its newest album's "added" */
} item_t;

static list_kind_t kind = LIST_TRACKS;
static char        folder[1024];        /* LIST_FOLDER / LIST_ALBUMS: the folder; LIST_ALBUM: the album's folder */
static int         album_src = -1, album_idx = -1;   /* LIST_ALBUM */
static char        album_card[64];
static int         artist_idx = -1;     /* LIST_ARTIST_TRACKS: the artist (artists.h) */
static int         sort_override = -1;  /* LIST_ARTIST_TRACKS: the sort the opener asked for, until the picker changes it */
static item_t *    items;
static int         nitems;
static unsigned    lib_gen, hist_gen;
static bool        hist_dirty;

/* ---- sections (the jump stops) ---- */

typedef struct { char label[PICKER_LABEL_LEN]; int first, count, group; tile_style_t style; } section_t;
static section_t * sections;
static int         nsections;

/* ---- options ---- */

enum { STYLE_TILES, STYLE_LIST, STYLE_GRID, STYLE_COVERS, STYLE_ROWS };
static picker_opts_t view_opts;
static int  style_ids[PICKER_MAX_CHOICES];      /* the STYLE_* behind each picker style */
static int  sort_kind;                          /* LIB_SORT_* or SORT_FILE / SORT_PLAYED / SORT_ALBUMS */
#define SORT_FILE   100
#define SORT_PLAYED 101
#define SORT_ALBUMS 102                         /* an artist's tracks album by album (year order), then track order (0.20) */
static int  opens;                              /* LIST_ARTISTS: what an artist row opens as (the Sort row's place) */
static int  sort_ids[PICKER_MAX_CHOICES];
static bool visited, visited_before_open, suppress_overlay;

/* ---- screen ---- */

static lv_obj_t * scr, * square, * art_img, * field, * content, * jump_row, * jump_l, * jump_r, * jump_label, * msg;
static lv_timer_t * poll_timer, * index_timer, * settle_timer, * flash_timer;
static void (*exit_cb)(void);
static void (*play_cb)(void);
static void (*style_cb)(const char *);
static void (*open_album_cb)(const library_t *, int, const char *, const tile_style_t *);
static void (*open_folder_cb)(const char *);
static void (*open_artist_cb)(int);
static bool shown, tv, tiles, gridstyle, show_jump;
static unsigned seed;
static int  sel;                 /* -1 = the jump row */
static int  playing;             /* item index, -1 none */
static int  rh, rows_on, ncols, cell_w, cell_x0, cell_gap, cell_pad, tile_side, col_x, col_w, top_y, field_h, fx, fw, jump_h;
/* the Tracks grid (0.19a, Ian: "like want ads in the newspaper"): two
 * columns of coloured cells, each as tall as its title needs, in the
 * colours and faces the tiles are dealt */
static int *  cell_y;            /* per item: its top in the content */
static int *  cell_h;            /* per item: its height */
static signed char * cell_plan;  /* per item: the rung the title starts at (by its length) | lines guessed << 4 | tail << 7 */
static tile_style_t * cell_st;   /* per item: dealt colours + face */
static int    content_h, cell_unit;
static int    g_title_px, g_artist_px;
static int  f_title_px, f_artist_px;
static int  scroll_y, scroll_target;
static int  cur_section;         /* whose colourway the page wears */
static uint32_t c_bg, c_ink, c_play, c_sel;
static int  jump_sec;            /* the stop the jump row shows while selected */
static int  flash_dir;
static int  gen;                 /* thumbs request generation */

typedef struct {
    int        item;             /* -2 = free */
    lv_obj_t * obj, * tile, * img, * title, * artist, * mark;
    uint8_t *  px;
    lv_image_dsc_t dsc;
    int        px_side;
} row_t;
static row_t rows[MAX_ROWS];

/* the TV square's picture: the cover, or a drawn companion (the cassette
 * with the folder's name; or an album's cassette or record) */
static uint8_t * art_px;
static lv_image_dsc_t art_dsc;
static int art_item = -1;        /* the item whose picture this is (a folder's own picture) ... */
static int art_src = -1, art_album = -1;   /* ... or the album it is the cover of (0.19b: the same cover stays up across an album's tracks) */
static pthread_mutex_t sq_lock = PTHREAD_MUTEX_INITIALIZER;
static int sq_gen;
static struct { int gen, section, key; char design[32]; uint8_t * px; int w, h; bool ready; } sq_res;
static uint8_t * pic_px;
static int pic_section = -1, pic_key = -2;
static char pic_design[32];
static lv_image_dsc_t pic_dsc;
static lv_obj_t * pic_img, * notile;
/* the TV square as a mosaic of an artist's albums (LIST_ARTISTS, 0.20):
 * 1, 2x2 .. 5x5 cells, a cover or a small name tile each */
static lv_obj_t * mosaic;
static struct { lv_obj_t * obj, * img; uint8_t * px; lv_image_dsc_t dsc; } mcell[MOSAIC_MAX];
static int mosaic_item = -1, mosaic_n;

static int tvpx(int v) { return (v * ui_screen.stage + TV_BASE / 2) / TV_BASE; }
static const lv_font_t * jost(int px) { return face_px("Jost-Medium.ttf", px); }

/* ------------------------------------------------------------ items */

static const library_t * lib_of(int i) { return &libs_get(items[i].src)->lib; }
static const lib_track_t * track_of(int i) { return items[i].kind == ITEM_TRACK ? &lib_of(i)->tracks[items[i].track] : NULL; }
static const lib_album_t * album_of(int i) { return items[i].album >= 0 ? &lib_of(i)->albums[items[i].album] : NULL; }
static const lib_folder_t * folder_of(int i) { return items[i].kind == ITEM_FOLDER ? &lib_of(i)->folders[items[i].folder] : NULL; }
static const artist_t * artist_of(int i) { return items[i].kind == ITEM_ARTIST ? artists_get(items[i].artist) : NULL; }
static const char * card_of(int i) { return libs_get(items[i].src)->card; }

static const char * base_name(const char * uri)
{
    const char * slash = strrchr(uri, '/');
    return slash ? slash + 1 : uri;
}

/* An untagged track shows its file name (Ian, 13 Oct: "some tracks have
 * the title of their folder" - the album's name told nothing apart). */
static const char * file_title(const lib_track_t * t)
{
    static char buf[300];
    snprintf(buf, sizeof(buf), "%.299s", base_name(t->uri));
    char * dot = strrchr(buf, '.');
    if(dot && dot > buf && strlen(dot) <= 5) *dot = 0;
    return buf;
}

static const char * item_title(int i)
{
    if(items[i].kind == ITEM_HEADER) return album_of(i) ? album_of(i)->title : "";
    const lib_track_t * t = track_of(i);
    if(t) return t->title[0] ? t->title : file_title(t);
    const lib_folder_t * f = folder_of(i);
    if(f) return base_name(f->uri);
    const artist_t * ar = artist_of(i);
    if(ar) return ar->name;
    return album_of(i) ? album_of(i)->title : "";
}

static const char * album_artist_text(const lib_album_t * a)
{
    return a->various ? T(S_VARIOUS_ARTISTS) : a->artist;
}

static const char * item_artist(int i)
{
    const lib_track_t * t = track_of(i);
    if(t) {
        if(t->artist[0]) return t->artist;
        const lib_album_t * a = album_of(i);
        return a ? album_artist_text(a) : "";
    }
    if(folder_of(i) || artist_of(i)) {
        static char buf[64], n[16];
        if(items[i].count <= 0) return "";
        if(items[i].count == 1) return T(S_LIST_ALBUM_ONE);
        snprintf(n, sizeof(n), "%d", items[i].count);
        ui_strf(buf, sizeof(buf), S_LIST_ALBUMS_COUNT, "n", n, NULL);
        return buf;
    }
    return album_of(i) ? album_artist_text(album_of(i)) : "";
}

/* An artist's picture for a small tile: their own, else the cover of the
 * first album of theirs that has one (0.20). */
static bool artist_art_path(int i, int px, char * out, size_t len)
{
    const artist_t * ar = artist_of(i);
    if(!ar) return false;
    if(items[i].pic) { snprintf(out, len, "%s", items[i].pic); return true; }
    for(int k = 0; k < ar->nalbums; k++) {
        const lib_album_t * a = &libs_get(ar->albums[k].src)->lib.albums[ar->albums[k].album];
        if(!a->has_art) continue;
        int si = 0;
        while(si < LIB_THUMB_COUNT - 1 && lib_thumb_px[si] < px) si++;
        while(si > 0 && !(a->thumb_sizes & (1 << si))) si--;
        cache_thumb_path(libs_get(ar->albums[k].src)->card, a, si, out, len);
        return true;
    }
    return false;
}

/* The line under a row's title: the artist, or in one artist's tracks
 * the album (Ian, 14 Oct: the artist is the same all the way down). */
static const char * item_subline(int i)
{
    if(kind == LIST_ARTIST_TRACKS && track_of(i) && album_of(i)) return album_of(i)->title;
    if(items[i].kind == ITEM_ARTIST) {
        /* albums, or tracks when an artist opens as tracks (Ian, 14 Oct) */
        static char buf[64];
        return artists_count_text(items[i].artist, opens != 0, buf, sizeof(buf));
    }
    return item_artist(i);
}

static bool item_has_art(int i)
{
    if(items[i].kind == ITEM_HEADER) return false;
    if(items[i].kind == ITEM_FOLDER) return items[i].pic != NULL;
    if(items[i].kind == ITEM_ARTIST) { char p[1024]; return artist_art_path(i, 192, p, sizeof(p)); }
    const lib_album_t * a = album_of(i);
    return a && a->has_art;
}

static int  item_year(int i) { const lib_album_t * a = album_of(i); return a ? a->year : 0; }

static long item_added(int i)
{
    const lib_track_t * t = track_of(i);
    if(t) return t->added ? t->added : t->mtime;
    if(folder_of(i)) return items[i].newest;
    const lib_album_t * a = album_of(i);
    return a ? a->newest : 0;
}

static const char * item_uri(int i)
{
    const lib_track_t * t = track_of(i);
    if(t) return t->uri;
    const lib_folder_t * f = folder_of(i);
    if(f) return f->uri;
    const artist_t * ar = artist_of(i);
    if(ar) return ar->name;
    return album_of(i) ? album_of(i)->folder : "";
}

static const char * view_id(void)
{
    switch(kind) {
        case LIST_RECENT: return "recent";
        case LIST_FOLDER: case LIST_ALBUM: return "mixtape";
        case LIST_ALBUMS: return "folder";
        case LIST_ARTISTS: return "artists";             /* shared with rows.c and the grid: one style key */
        case LIST_ARTIST_TRACKS: return "artist-tracks";
        default: return "tracks";
    }
}

const char * listpage_view_id(void) { return view_id(); }

/* The Various Artists row sits last, outside the alphabet: the end
 * section, labelled "VA" rather than "?". */
static bool is_various_row(int i) { const artist_t * ar = artist_of(i); return ar && ar->various; }
static bool end_label(const char * l) { return strcmp(l, "?") == 0 || strcmp(l, "VA") == 0; }

static bool lacks(int i)
{
    if(is_various_row(i)) return true;
    switch(sort_kind) {
        case LIB_SORT_ARTIST_TITLE: case LIB_SORT_ARTIST_YEAR: return items[i].kind != ITEM_FOLDER && item_artist(i)[0] == 0;
        case LIB_SORT_YEAR: return item_year(i) == 0;
        default: return false;
    }
}

static int track_order_cmp(int ia, int ib)
{
    const lib_track_t * s = track_of(ia), * t = track_of(ib);
    if(s && t && kind == LIST_ALBUM) {
        if(s->disc != t->disc) return s->disc < t->disc ? -1 : 1;
        if(s->track != t->track) return s->track < t->track ? -1 : 1;
    }
    return strcmp(item_uri(ia), item_uri(ib));
}

static int item_cmp(const void * pa, const void * pb)
{
    const item_t * a = pa, * b = pb;
    int ia = (int)(a - items), ib = (int)(b - items);
    int c = 0;
    if(sort_kind == SORT_FILE) c = track_order_cmp(ia, ib);
    else if(sort_kind == SORT_PLAYED) c = 0;
    else if(sort_kind == SORT_ALBUMS) {
        /* the artist's albums in their (year) order, each in disc / track order */
        c = a->artist - b->artist;
        if(!c && (a->kind == ITEM_HEADER) != (b->kind == ITEM_HEADER)) return a->kind == ITEM_HEADER ? -1 : 1;
        const lib_track_t * s = track_of(ia), * t = track_of(ib);
        if(!c && s && t) {
            if(s->disc != t->disc) c = s->disc < t->disc ? -1 : 1;
            else if(s->track != t->track) c = s->track < t->track ? -1 : 1;
        }
    }
    else {
        bool la = lacks(ia), lb = lacks(ib);
        if(la != lb) return la ? 1 : -1;
        if(la) c = strcmp(item_uri(ia), item_uri(ib));
        else switch(sort_kind) {
            case LIB_SORT_ARTIST_TITLE:
                c = library_name_cmp(item_artist(ia), item_artist(ib));
                if(!c) c = library_name_cmp(item_title(ia), item_title(ib));
                break;
            case LIB_SORT_ARTIST_YEAR: {
                c = library_name_cmp(item_artist(ia), item_artist(ib));
                const lib_album_t * x = album_of(ia), * y = album_of(ib);
                if(!c && x && y) c = library_name_cmp(x->title, y->title);
                if(!c) c = track_order_cmp(ia, ib);
                break;
            }
            case LIB_SORT_YEAR:
                c = item_year(ia) - item_year(ib);
                if(!c) c = library_name_cmp(item_artist(ia), item_artist(ib));
                if(!c) c = library_name_cmp(item_title(ia), item_title(ib));
                break;
            case LIB_SORT_NEWEST:
                c = item_added(ia) > item_added(ib) ? -1 : item_added(ia) < item_added(ib) ? 1 : 0;
                break;
            default:
                c = library_name_cmp(item_title(ia), item_title(ib));
                if(!c) c = library_name_cmp(item_artist(ia), item_artist(ib));
                break;
        }
    }
    if(!c) c = strcmp(item_uri(ia), item_uri(ib));
    if(!c) c = a->src - b->src;
    return c;
}

/* ---- sections ---- */

/* The letters a name sorts under: its first, and its second (for the
 * split of a letter into stops), folded as the picker folds - both off the
 * name's sort key, so "(What's the Story)" is a W and "01 - Song" an S. */
static void first_letters(const char * name, char l1[PICKER_LABEL_LEN], char l2[PICKER_LABEL_LEN])
{
    const char * s = library_sort_key(name);
    picker_fold(s, l1);
    const char * p = s;
    if(*p) { p++; while(*p && ((unsigned char)*p & 0xC0) == 0x80) p++; }
    while(*p && (unsigned char)*p < 0x80 && !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) p++;
    if(*p) picker_fold(p, l2); else l2[0] = 0;
}

static const char * key_name(int i)
{
    bool by_artist = sort_kind == LIB_SORT_ARTIST_TITLE || sort_kind == LIB_SORT_ARTIST_YEAR;
    return by_artist && items[i].kind != ITEM_FOLDER ? item_artist(i) : item_title(i);
}

static void sections_free(void) { free(sections); sections = NULL; nsections = 0; }

static void section_add(const char * label, int first)
{
    sections = realloc(sections, sizeof(section_t) * (size_t)(nsections + 1));
    if(!sections) abort();
    memset(&sections[nsections], 0, sizeof(section_t));
    snprintf(sections[nsections].label, PICKER_LABEL_LEN, "%s", label);
    sections[nsections].first = first;
    nsections++;
}

static void sections_renumber(void)
{
    for(int s = 0; s < nsections; s++) {
        int end = s + 1 < nsections ? sections[s + 1].first : nitems;
        sections[s].count = end - sections[s].first;
        for(int k = sections[s].first; k < end; k++) items[k].section = s;
    }
}

static void section_remove(int s)
{
    memmove(&sections[s], &sections[s + 1], sizeof(section_t) * (size_t)(nsections - s - 1));
    nsections--;
}

/* Whether there is anything to jump between: two stops that aren't the
 * end section (a short list merged into one letter plus "VA" has none). */
static bool jumpable(void)
{
    int real = 0;
    for(int s = 0; s < nsections; s++) if(!end_label(sections[s].label)) real++;
    return real >= 2;
}

/* An artist's tracks in album order: the stop is the album's year, or
 * its title's letter when it has none (0.20). */
static void album_label(int i, char out[PICKER_LABEL_LEN])
{
    const lib_album_t * a = album_of(i);
    if(a && a->year) snprintf(out, PICKER_LABEL_LEN, "%d", a->year);
    else { char l2[PICKER_LABEL_LEN]; first_letters(a ? a->title : item_title(i), out, l2); }
}

/* Jump stops (Ian, 13 Oct): letters, or years; a letter with more than
 * about CHUNK_PAGES screens of tracks is split at second-letter boundaries
 * into up to CHUNK_MAX stops, each labelled with its first two letters; a
 * stop with less than a screenful joins its neighbour (13 Oct evening:
 * "letters that don't even have a whole page of tracks shouldn't be on the
 * picker"). Every stop of one letter (year, artist's letter) is one colour
 * group. */
static void build_sections(void)
{
    sections_free();
    if(nitems == 0) return;
    if(sort_kind == LIB_SORT_NEWEST || sort_kind == SORT_FILE || sort_kind == SORT_PLAYED) {
        section_add("\xE2\x80\xA2", 0);      /* one section, a dot: nothing to jump by */
        sections_renumber();
        sections[0].group = 0;
        view_opts.label_count = 0;
        view_opts.jump = -1;
        return;
    }
    int per_screen = rows_on > 0 ? rows_on * (ncols > 0 ? ncols : 1) : 8;
    int chunk = CHUNK_PAGES * per_screen;
    int i = 0;
    while(i < nitems) {
        /* a run of one first letter (or one year) */
        char l1[PICKER_LABEL_LEN], l2[PICKER_LABEL_LEN], lx[PICKER_LABEL_LEN], ly[PICKER_LABEL_LEN];
        bool end = lacks(i);
        if(end) { snprintf(l1, sizeof(l1), "%s", is_various_row(i) ? "VA" : "?"); }
        else if(sort_kind == LIB_SORT_YEAR) snprintf(l1, sizeof(l1), "%d", item_year(i));
        else if(sort_kind == SORT_ALBUMS) album_label(i, l1);
        else first_letters(key_name(i), l1, l2);
        int j = i + 1;
        while(j < nitems) {
            if(lacks(j) != end) break;
            if(!end) {
                if(sort_kind == LIB_SORT_YEAR) snprintf(lx, sizeof(lx), "%d", item_year(j));
                else if(sort_kind == SORT_ALBUMS) album_label(j, lx);
                else first_letters(key_name(j), lx, ly);
                if(strcmp(lx, l1) != 0) break;
            }
            j++;
        }
        int run = j - i;
        int parts = (run + chunk - 1) / chunk;
        if(parts > CHUNK_MAX) parts = CHUNK_MAX;
        if(parts <= 1 || end || sort_kind == LIB_SORT_YEAR || sort_kind == SORT_ALBUMS) section_add(l1, i);
        else {
            /* cut at the second-letter boundaries nearest the quarter points */
            int cuts[CHUNK_MAX + 1];
            cuts[0] = i;
            int ncut = 1;
            for(int p = 1; p < parts; p++) {
                int want = i + run * p / parts, best = -1, bestd = 1 << 30;
                for(int k = i + 1; k < j; k++) {
                    char a1[PICKER_LABEL_LEN], a2[PICKER_LABEL_LEN], b1[PICKER_LABEL_LEN], b2[PICKER_LABEL_LEN];
                    first_letters(key_name(k - 1), a1, a2);
                    first_letters(key_name(k), b1, b2);
                    if(strcmp(a2, b2) == 0) continue;
                    int d = k > want ? k - want : want - k;
                    if(d < bestd) { bestd = d; best = k; }
                }
                if(best > cuts[ncut - 1]) cuts[ncut++] = best;
            }
            cuts[ncut] = j;
            for(int p = 0; p < ncut; p++) {
                char label[PICKER_LABEL_LEN];
                if(p == 0) snprintf(label, sizeof(label), "%s", l1);
                else {
                    char b1[PICKER_LABEL_LEN], b2[PICKER_LABEL_LEN];
                    first_letters(key_name(cuts[p]), b1, b2);
                    char low[PICKER_LABEL_LEN];
                    snprintf(low, sizeof(low), "%s", b2);
                    if(low[0] >= 'A' && low[0] <= 'Z' && !low[1]) low[0] = (char)(low[0] + 32);
                    snprintf(label, sizeof(label), "%s%s", l1, low);
                }
                section_add(label, cuts[p]);
            }
        }
        i = j;
    }
    sections_renumber();
    /* a stop short of a screenful joins the one before it (the first one
     * takes the next in and its label); "?" stays its own */
    bool merged = true;
    while(merged && nsections > 1 && rows_on > 0 && sort_kind != SORT_ALBUMS) {   /* album order: every album its own stop, however short */
        merged = false;
        for(int s = 0; s < nsections; s++) {
            if(sections[s].count >= per_screen || end_label(sections[s].label)) continue;
            if(s > 0 && !end_label(sections[s - 1].label)) { section_remove(s); merged = true; break; }
            if(s + 1 < nsections && !end_label(sections[s + 1].label)) {
                /* the first stop takes the next in, and its label when that is the bigger part */
                if(sections[s + 1].count > sections[s].count) snprintf(sections[s].label, PICKER_LABEL_LEN, "%s", sections[s + 1].label);
                section_remove(s + 1);
                merged = true;
                break;
            }
        }
        if(merged) sections_renumber();
    }
    /* colour groups: the stops of one letter share one */
    int ngroups = 0;
    for(int s = 0; s < nsections; s++) {
        char head[PICKER_LABEL_LEN];
        snprintf(head, sizeof(head), "%s", sections[s].label);
        bool whole = sort_kind == LIB_SORT_YEAR || sort_kind == SORT_ALBUMS || end_label(head);
        if(!whole) {
            /* the first UTF-8 character */
            int n = 1;
            while(head[n] && ((unsigned char)head[n] & 0xC0) == 0x80) n++;
            head[n] = 0;
        }
        int g = -1;
        for(int k = 0; k < s && g < 0; k++) {
            char hk[PICKER_LABEL_LEN];
            snprintf(hk, sizeof(hk), "%s", sections[k].label);
            if(!(sort_kind == LIB_SORT_YEAR || sort_kind == SORT_ALBUMS || end_label(hk))) { int n = 1; while(hk[n] && ((unsigned char)hk[n] & 0xC0) == 0x80) n++; hk[n] = 0; }
            if(strcmp(hk, head) == 0) g = sections[k].group;
        }
        sections[s].group = g >= 0 ? g : ngroups++;
    }
    /* the picker's jump row shows the same stops (none when there is one
     * stop - or one stop and the end section - nothing to jump to) */
    view_opts.label_count = !jumpable() ? 0 : nsections < PICKER_MAX_LABELS ? nsections : PICKER_MAX_LABELS;
    for(int s = 0; s < view_opts.label_count; s++) snprintf(view_opts.labels[s], PICKER_LABEL_LEN, "%s", sections[s].label);
    view_opts.jump = view_opts.label_count ? 0 : -1;
}

/* A colourway per letter group: dealt in one column at the list contrast,
 * then turned the way Ian asked (one colour saturated, the other light;
 * never black on white) when the deal didn't come out so. */
static void deal_sections(void)
{
    if(nsections == 0) return;
    int ngroups = 0;
    for(int s = 0; s < nsections; s++) if(sections[s].group + 1 > ngroups) ngroups = sections[s].group + 1;
    tile_style_t * st = calloc((size_t)ngroups, sizeof(tile_style_t));
    if(!st) return;
    tilestyle_deal_list(seed, ngroups, st);
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0;
    for(int g = 0; g < ngroups; g++) {
        uint32_t bg = st[g].bg, ink = st[g].ink;
        double sb = tilestyle_saturation(bg), si = tilestyle_saturation(ink);
        bool bg_light = tilestyle_contrast(bg, 0x000000) > tilestyle_contrast(bg, 0xFFFFFF);
        bool ink_light = tilestyle_contrast(ink, 0x000000) > tilestyle_contrast(ink, 0xFFFFFF);
        bool ok = (bg_light && si >= 0.35) || (ink_light && sb >= 0.35);
        if(!ok) {
            uint32_t set[16];
            int n = tilestyle_set_colours(bg, set, 16);
            uint32_t best = 0; double bests = 0;
            for(int k = 0; k < n; k++)
                if(tilestyle_contrast(bg, set[k]) >= tilestyle_list_contrast() && tilestyle_saturation(set[k]) > bests) { bests = tilestyle_saturation(set[k]); best = set[k]; }
            if(best && bests >= 0.35) ink = best;
            else if(sb >= 0.35 && tilestyle_contrast(bg, cream) >= tilestyle_list_contrast()) ink = cream;
            st[g].ink = ink;
        }
    }
    for(int s = 0; s < nsections; s++) sections[s].style = st[sections[s].group];
    free(st);
}

/* ------------------------------------------------------------ options */

static void view_key(const char * what, char * out, size_t len)
{
    snprintf(out, len, "%s-%s-%s", what, view_id(), ui_screen_kind_name());
}

static void add_style(int id, int str) { view_opts.styles[view_opts.style_count] = T(str); style_ids[view_opts.style_count++] = id; }
static void add_sort(int id, int str)  { view_opts.sorts[view_opts.sort_count] = T(str);   sort_ids[view_opts.sort_count++] = id; }

static void read_options(void)
{
    char key[64], v[24];
    view_opts.style_count = 0;
    if(kind == LIST_ARTISTS) add_style(STYLE_ROWS, S_STYLE_ROWS);           /* the artists' other faces: rows.c and the grid */
    if(kind == LIST_ALBUMS || kind == LIST_ARTISTS) add_style(STYLE_COVERS, S_STYLE_GRID);   /* the folder grid: this page's other face */
    if(!tv) add_style(STYLE_TILES, S_STYLE_TILES);
    add_style(STYLE_LIST, S_STYLE_LIST);
    if(kind == LIST_TRACKS || kind == LIST_ARTIST_TRACKS) add_style(STYLE_GRID, S_STYLE_GRID);
    view_key("style", key, sizeof(key));
    config_read(key, v, sizeof(v), tv ? "list" : "tiles");
    view_opts.style = -1;
    for(int i = 0; i < view_opts.style_count; i++) {
        int id = style_ids[i];
        if((id == STYLE_TILES && strcmp(v, "tiles") == 0) || (id == STYLE_LIST && strcmp(v, "list") == 0) ||
           ((id == STYLE_GRID || id == STYLE_COVERS) && strcmp(v, "grid") == 0) || (id == STYLE_ROWS && strcmp(v, "rows") == 0)) view_opts.style = i;
    }
    /* a folder's "grid" (an artist's "rows") belongs elsewhere: here it reads as the rows */
    if(view_opts.style < 0 || style_ids[view_opts.style] == STYLE_COVERS || style_ids[view_opts.style] == STYLE_ROWS) {
        view_opts.style = 0;
        for(int i = 0; i < view_opts.style_count; i++) if(style_ids[i] == (tv ? STYLE_LIST : STYLE_TILES)) view_opts.style = i;
    }
    tiles = !tv && style_ids[view_opts.style] == STYLE_TILES;
    gridstyle = style_ids[view_opts.style] == STYLE_GRID;

    view_key("size", key, sizeof(key));
    config_read(key, v, sizeof(v), ui_size_name(UI_SIZE_DEFAULT));
    view_opts.size = UI_SIZE_DEFAULT;
    for(int i = 0; i < UI_SIZE_COUNT; i++) if(strcmp(ui_size_name((ui_size_t)i), v) == 0 && ui_size_offered((ui_size_t)i)) view_opts.size = (ui_size_t)i;

    view_opts.sort_count = 0;
    view_opts.sort_name = NULL;
    switch(kind) {
        case LIST_RECENT: add_sort(SORT_PLAYED, S_SORT_LAST_PLAYED); break;
        case LIST_FOLDER: add_sort(SORT_FILE, S_SORT_FILE_ORDER); add_sort(LIB_SORT_TITLE, S_SORT_TITLE); add_sort(LIB_SORT_ARTIST_TITLE, S_SORT_ARTIST_TITLE); break;
        case LIST_ALBUM:  add_sort(SORT_FILE, S_SORT_TRACK_ORDER); add_sort(LIB_SORT_TITLE, S_SORT_TITLE); add_sort(LIB_SORT_ARTIST_TITLE, S_SORT_ARTIST_TITLE); break;
        case LIST_ALBUMS: add_sort(LIB_SORT_TITLE, S_SORT_NAME); add_sort(LIB_SORT_NEWEST, S_SORT_NEWEST); break;
        case LIST_ARTISTS: add_sort(LIB_SORT_TITLE, S_SORT_NAME); break;      /* by name only; the row is "Opens as" below */
        case LIST_ARTIST_TRACKS: add_sort(LIB_SORT_TITLE, S_SORT_TITLE); add_sort(SORT_ALBUMS, S_SORT_ALBUM_ORDER); add_sort(LIB_SORT_NEWEST, S_SORT_NEWEST); break;   /* one artist: "Album order", not "Artist, album" (Ian, 14 Oct) */
        default:
            add_sort(LIB_SORT_TITLE, S_SORT_TITLE); add_sort(LIB_SORT_ARTIST_TITLE, S_SORT_ARTIST_TITLE);
            add_sort(LIB_SORT_ARTIST_YEAR, S_SORT_ARTIST_ALBUM); add_sort(LIB_SORT_YEAR, S_SORT_YEAR); add_sort(LIB_SORT_NEWEST, S_SORT_NEWEST);
            break;
    }
    view_key("sort", key, sizeof(key));
    config_read(key, v, sizeof(v), kind == LIST_FOLDER || kind == LIST_ALBUM ? "file" : kind == LIST_RECENT ? "played" : "title");
    view_opts.sort = 0;
    for(int i = 0; i < view_opts.sort_count; i++) {
        const char * name = sort_ids[i] == SORT_FILE ? "file" : sort_ids[i] == SORT_PLAYED ? "played" : sort_ids[i] == SORT_ALBUMS ? "albums" : library_sort_name((lib_sort_t)sort_ids[i]);
        if(strcmp(name, v) == 0) view_opts.sort = i;
    }
    if(kind == LIST_ARTIST_TRACKS && sort_override >= 0)
        for(int i = 0; i < view_opts.sort_count; i++) if(sort_ids[i] == sort_override) view_opts.sort = i;   /* what "Opens as" asked for */
    sort_kind = sort_ids[view_opts.sort];
    if(kind == LIST_ARTISTS) {
        /* the Sort row reads "Opens as": what Select on an artist opens
         * (albums, their tracks by title, their tracks by album) - the
         * same setting the rows and grid faces keep */
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
    view_opts.jump_row = sort_kind != SORT_FILE && sort_kind != SORT_PLAYED && sort_kind != LIB_SORT_NEWEST;
    view_opts.names_row = false;
    view_key("visited", key, sizeof(key));
    visited = config_read_bool(key, false);
}

static void write_options(void)
{
    char key[64];
    int id = style_ids[view_opts.style];
    view_key("style", key, sizeof(key));
    config_write(key, id == STYLE_TILES ? "tiles" : id == STYLE_LIST ? "list" : id == STYLE_ROWS ? "rows" : "grid");
    view_key("size", key, sizeof(key));
    config_write(key, ui_size_name(view_opts.size));
    if(kind == LIST_ARTISTS) { view_key("opens", key, sizeof(key)); config_write(key, opens == 1 ? "tracks" : opens == 2 ? "tracks-album" : "albums"); }
    else { view_key("sort", key, sizeof(key)); config_write(key, sort_kind == SORT_FILE ? "file" : sort_kind == SORT_PLAYED ? "played" : sort_kind == SORT_ALBUMS ? "albums" : library_sort_name((lib_sort_t)sort_kind)); }
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

/* ------------------------------------------------------------ measure */

static void measure(void)
{
    tv = ui_screen.kind == UI_SCREEN_TV;
    /* the size row scales the rows: M the design, S smaller, L / XL / XXL bigger */
    int pct = view_opts.size == UI_SIZE_S ? 85 : view_opts.size == UI_SIZE_L ? 100 : view_opts.size == UI_SIZE_XL ? 115 : view_opts.size == UI_SIZE_XXL ? 130 : 92;
    int margin;
    if(tv) {
        int stage = ui_screen.stage, rest = ui_screen.w - stage;
        rh = tvpx(104) * pct / 100;
        f_title_px = tvpx(38) * pct / 100;
        f_artist_px = tvpx(26) * pct / 100;
        margin = tvpx(70);
        fw = rest;
        col_x = margin;
        col_w = rest - 2 * margin;
        top_y = ui_screen.stage_y + tvpx(60);
        field_h = stage - tvpx(100);
        tile_side = 0;
    }
    else if(tiles) {
        ui_grid_t g;
        ui_grid_metrics(UI_SIZE_M, &g);
        rh = g.cell * pct / 100;
        tile_side = rh * 91 / 100;
        f_title_px = PX(40) * pct / 100;
        f_artist_px = PX(27) * pct / 100;
        margin = PX(16);
        fw = ui_screen.stage;
        col_x = PX(16) + tile_side + PX(26);
        col_w = ui_screen.stage - col_x - PX(24);
        top_y = ui_screen.stage_y;
        field_h = ui_screen.stage;
    }
    else {
        ui_list_t l;
        ui_list_metrics(UI_SIZE_L, &l);
        rh = (l.row_h + l.row_gap) * pct / 100;
        tile_side = 0;
        f_title_px = PX(32) * pct / 100;
        f_artist_px = PX(22) * pct / 100;
        margin = PX(16);
        fw = ui_screen.stage;
        col_x = PX(40);
        col_w = ui_screen.stage - 2 * PX(40);
        top_y = ui_screen.stage_y;
        field_h = ui_screen.stage;
    }
    show_jump = tv && view_opts.jump_row && jumpable();
    jump_h = show_jump ? rh : 0;
    ncols = 1;
    cell_x0 = 0;
    cell_w = fw;
    if(gridstyle) {
        /* the Tracks grid: GRID_COLS columns of cells, each as tall as its
         * title asks (cell_layout); rh stays the list row for the jump row */
        ncols = GRID_COLS;
        cell_x0 = margin;
        cell_w = (fw - 2 * margin) / ncols;
        cell_gap = tv ? tvpx(12) : PX(8);
        cell_pad = tv ? tvpx(16) : PX(12);
        tile_side = 0;
        g_title_px = (tv ? tvpx(44) : PX(36)) * pct / 100;       /* a short title's size; longer ones start smaller (Ian, 23:53) */
        g_artist_px = (tv ? tvpx(27) : PX(22)) * pct / 100;
    }
    rows_on = (field_h - jump_h) / rh;
    if(rows_on < 1) rows_on = 1;
}

/* ---- the want-ads cells: positions and heights ---- */

static const char * cell_face(int item, bool * caps, int * leading)
{
    const tile_font_t * f = (cell_st && cell_st[item].font != TILE_FONT_NONE) ? tilestyle_font(cell_st[item].font) : NULL;
    if(caps) *caps = f ? f->caps : false;
    if(leading) *leading = f ? f->leading : 100;
    return f ? f->file : "Jost-Medium.ttf";
}

static int cell_inner_w(void) { return cell_w - cell_gap - 2 * cell_pad; }

/* The rung a title starts at, by its length: a short title big, a long one
 * smaller from the start (Ian, 13 Oct 23:53 - it had been the other way
 * round); then it shrinks only as far as whole words need. */
static int title_rung(int letters)
{
    return letters <= 10 ? 0 : letters <= 16 ? 1 : letters <= 24 ? 2 : letters <= 36 ? 3 : 4;
}

/* Lay the cells out: item i in column i % GRID_COLS, each column a stack.
 * Heights are whole units (a unit is about half a one-line cell), so the
 * two columns' corners meet whenever their units add up the same (Ian:
 * "they often fall very close but never line up"): a one-line title is 2
 * units, each further line one more, a bracketed tail one more. The lines
 * are guessed from the title's length in its face at its starting rung;
 * the real fit happens when the cell is made, inside that height. */
static void cell_layout(void)
{
    free(cell_y); free(cell_h); free(cell_plan);
    cell_y = calloc((size_t)(nitems > 0 ? nitems : 1), sizeof(int));
    cell_h = calloc((size_t)(nitems > 0 ? nitems : 1), sizeof(int));
    cell_plan = calloc((size_t)(nitems > 0 ? nitems : 1), sizeof(signed char));
    content_h = 0;
    if(!cell_y || !cell_h || !cell_plan || nitems == 0) return;
    int col_top[GRID_COLS] = { 0 };
    int ah = lv_font_get_line_height(jost(g_artist_px));
    int th = lv_font_get_line_height(jost(g_title_px));
    cell_unit = (2 * cell_pad + th + cell_pad / 2 + ah + cell_gap + 1) / 2;
    int inner_w = cell_inner_w();
    int lad[LADDER_STEPS];
    ladder_fill(lad, g_title_px);
    /* per face and rung: an average letter width, measured once */
    int nf = tilestyle_font_count() + 1;
    int * cw = calloc((size_t)nf * 5, sizeof(int));
    for(int i = 0; i < nitems; i++) {
        int fi = (cell_st && cell_st[i].font != TILE_FONT_NONE) ? cell_st[i].font + 1 : 0;
        if(fi >= nf) fi = 0;
        char main[300], br[300];
        const char * title = item_title(i);
        bool tail = split_bracket(title, main, sizeof(main), br, sizeof(br));
        const char * text = tail ? main : title;
        int letters = count_letters(text);
        int rung = title_rung(letters);
        int slot = fi * 5 + rung;
        if(cw && cw[slot] == 0) {
            int leading;
            const char * file = cell_face(i, NULL, &leading);
            const lv_font_t * f = face_px(file, lad[rung]);
            cw[slot] = text_w(f, "The Quick Brown Fox Jumps Over The Lazy Dog And Runs Away") * 11 / 10 / 57;
            if(cw[slot] < 4) cw[slot] = 4;
        }
        int lw = cw ? cw[slot] : lad[rung] * 6 / 10;
        int tw = (int)strlen(text) * lw;
        int lines = inner_w > 0 ? (tw + inner_w - 1) / inner_w : 1;
        if(lines < 1) lines = 1;
        if(lines > 3) lines = 3;
        int units = 1 + lines + (tail ? 1 : 0);
        cell_plan[i] = (signed char)(rung | (lines << 4) | (tail ? 0x80 : 0));
        int h = units * cell_unit - cell_gap;
        int c = i % GRID_COLS;
        cell_y[i] = col_top[c];
        cell_h[i] = h;
        col_top[c] += h + cell_gap;
        if(col_top[c] > content_h) content_h = col_top[c];
    }
    free(cw);
}

static void cells_free(void)
{
    free(cell_y); free(cell_h); free(cell_st); free(cell_plan);
    cell_y = cell_h = NULL; cell_st = NULL; cell_plan = NULL;
    content_h = 0;
}

static int rows_total(void) { return ncols ? (nitems + ncols - 1) / ncols : 0; }
static int row_index(int item) { return item / ncols; }
static int col_index(int item) { return item % ncols; }
static int field_hh(void) { return field_h - jump_h; }
static int item_top(int item) { return gridstyle && cell_y ? cell_y[item] : row_index(item) * rh; }
static int item_height(int item) { return gridstyle && cell_h ? cell_h[item] : rh; }
static int list_height(void) { return gridstyle ? content_h : rows_total() * rh; }

/* ------------------------------------------------------------ colours */

static void apply_colours(int section)
{
    if(nsections == 0 || section < 0 || section >= nsections) { c_bg = UI_COLOR_BG; c_ink = UI_COLOR_FG; c_play = c_sel = UI_COLOR_FOCUS_BG; section = 0; }
    else {
        const tile_style_t * st = &sections[section].style;
        c_bg = st->bg;
        c_ink = st->ink;
        uint32_t p, s;
        tilestyle_page_accents(seed, c_bg, c_ink, &p, &s);
        c_play = tilestyle_distance(p, c_ink) >= 70 ? p : c_ink;
        c_sel = tilestyle_distance(s, c_ink) >= 70 && tilestyle_distance(s, c_play) >= 70 ? s : c_play;
    }
    cur_section = section;
    lv_obj_set_style_bg_color(scr, lv_color_hex(c_bg), 0);
    if(square) lv_obj_set_style_bg_color(square, lv_color_hex(c_bg), 0);
    if(jump_row) lv_obj_set_style_bg_color(jump_row, lv_color_hex(c_bg), 0);
}

/* ------------------------------------------------------------ rows */

static row_t * row_of(int item)
{
    for(int r = 0; r < MAX_ROWS; r++) if(rows[r].item == item) return &rows[r];
    return NULL;
}

static void row_free_px(row_t * r)
{
    if(r->px) { lv_image_cache_drop(&r->dsc); free(r->px); r->px = NULL; }
    r->px_side = 0;
}

static void row_delete(row_t * r)
{
    if(r->obj) lv_obj_delete(r->obj);
    row_free_px(r);
    memset(r, 0, sizeof(*r));
    r->item = -2;
}

static void row_style(row_t * r)
{
    if(!r || r->item < 0) return;
    if(items[r->item].kind == ITEM_HEADER) {
        /* an album's break: its title small, in the page's ink, a rule under it */
        lv_obj_set_style_text_color(r->title, lv_color_hex(c_ink), 0);
        if(r->mark) lv_obj_set_style_bg_color(r->mark, lv_color_hex(c_ink), 0);
        return;
    }
    bool is_sel = r->item == sel, is_play = r->item == playing;
    uint32_t col = is_play ? c_play : is_sel ? c_sel : c_ink;
    if(gridstyle) {
        /* a want-ad cell in its own dealt colours; the selected one turns
         * its colours round - the text in the background colour, the
         * background in the text's (Ian, 23:53: "unmissable") - and comes
         * forward with a shadow; the playing one wears a disc in its corner */
        uint32_t bg = cell_st ? cell_st[r->item].bg : c_bg, ink = cell_st ? cell_st[r->item].ink : c_ink;
        uint32_t cbg = is_sel ? ink : bg, cink = is_sel ? bg : ink;
        lv_obj_set_style_bg_color(r->obj, lv_color_hex(cbg), 0);
        lv_obj_set_style_shadow_width(r->obj, is_sel ? cell_pad * 2 : 0, 0);
        if(r->title) lv_obj_set_style_text_color(r->title, lv_color_hex(cink), 0);
        if(r->tile) lv_obj_set_style_text_color(r->tile, lv_color_hex(cink), 0);       /* the bracketed tail */
        if(r->artist) lv_obj_set_style_text_color(r->artist, lv_color_hex(cink), 0);
        if(r->mark) { lv_obj_set_style_bg_color(r->mark, lv_color_hex(cink), 0); if(is_play) lv_obj_remove_flag(r->mark, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(r->mark, LV_OBJ_FLAG_HIDDEN); }
        if(is_sel) lv_obj_move_foreground(r->obj);
        return;
    }
    lv_obj_set_style_text_font(r->title, jost(is_sel ? f_title_px * GROW_PCT / 100 : f_title_px), 0);
    lv_obj_set_style_text_color(r->title, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(r->artist, lv_color_hex(c_ink), 0);
    lv_obj_set_style_text_opa(r->artist, is_sel ? LV_OPA_COVER : LV_OPA_70, 0);
    if(r->mark) { if(is_play) lv_obj_remove_flag(r->mark, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(r->mark, LV_OBJ_FLAG_HIDDEN); }
    if(r->mark) lv_obj_set_style_bg_color(r->mark, lv_color_hex(c_play), 0);
    if(r->tile) {
        int side = is_sel ? tile_side * GROW_PCT / 100 : tile_side;
        lv_obj_set_pos(r->tile, PX(16) + (tile_side - side) / 2, (rh - side) / 2);
        lv_obj_set_size(r->tile, side, side);
        lv_obj_set_style_shadow_width(r->tile, is_sel ? side / 7 : 0, 0);
        if(lv_obj_get_child_count(r->tile) > 1) {
            /* the name tile's text box grows with the tile */
            lv_obj_t * box = lv_obj_get_child(r->tile, 1);
            if(box) { lv_obj_set_pos(box, (side - tile_side) / 2, (side - tile_side) / 2); lv_obj_set_style_transform_scale(box, 256 * side / tile_side, 0); }
        }
        if(is_sel) lv_obj_move_foreground(r->obj);
    }
}

static void row_want_art(void);

/* The small name tile for an item without art: a companion of the
 * section's colourway, keyed by the album (or folder) so it is the same
 * tile wherever that album shows. */
static void companion_style(int item, tile_style_t * out)
{
    unsigned k = (unsigned)(items[item].album >= 0 ? items[item].album + 1 : items[item].kind == ITEM_ARTIST ? 100000 + items[item].artist : -(items[item].folder + 2)) * 7919u;
    tilestyle_companion(seed + k, &sections[items[item].section].style, out);
}

static row_t * row_create(int item)
{
    row_t * r = NULL;
    for(int k = 0; k < MAX_ROWS; k++) if(rows[k].item == -2) { r = &rows[k]; break; }
    if(!r) return NULL;
    memset(r, 0, sizeof(*r));
    r->item = item;
    r->obj = lv_obj_create(content);
    lv_obj_remove_style_all(r->obj);
    lv_obj_remove_flag(r->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    if(gridstyle) {
        /* a want-ad: the title in the cell's dealt face, starting at the
         * rung its length earns, whole words, shrinking only as far as the
         * cell's width and lines ask; a bracketed tail smaller on its own
         * line under it; the artist in small caps under that; centred, as
         * the no-art tiles are */
        int h = item_height(item), inner_w = cell_inner_w();
        int plan = cell_plan ? cell_plan[item] : 0;
        int rung0 = plan & 0x0F, lines_est = (plan >> 4) & 0x07;
        bool has_tail = (plan & 0x80) != 0;
        if(lines_est < 1) lines_est = 1;
        lv_obj_set_pos(r->obj, cell_x0 + col_index(item) * cell_w + cell_gap / 2, item_top(item));
        lv_obj_set_size(r->obj, cell_w - cell_gap, h);
        lv_obj_set_style_bg_opa(r->obj, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(r->obj, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(r->obj, LV_OPA_50, 0);
        bool caps; int leading;
        const char * file = cell_face(item, &caps, &leading);
        char main[300], br[300];
        if(!split_bracket(item_title(item), main, sizeof(main), br, sizeof(br))) { snprintf(main, sizeof(main), "%s", item_title(item)); br[0] = 0; has_tail = false; }
        if(caps) { caps_inplace(main); caps_inplace(br); }
        const lv_font_t * fa = jost(g_artist_px);
        int ah = lv_font_get_line_height(fa);
        int room = h - 2 * cell_pad - cell_pad / 2 - ah;
        int lad[LADDER_STEPS];
        ladder_fill(lad, g_title_px);
        int floor_k = ladder_floor(lad, TEXT_MIN_PX);
        fit_t fm = { 0 }, fb = { 0 };
        int km = fit_rung(file, leading, lad, rung0, floor_k, main, inner_w, room - (has_tail ? cell_unit / 2 : 0), lines_est + 1, &fm);
        if(km < 0) { fit_fallback(file, leading, inner_w, room - (has_tail ? cell_unit / 2 : 0), lines_est + 1, main, false, &fm); km = floor_k; }
        bool show_tail = has_tail && br[0] && fit_tail(file, leading, inner_w, inner_w, room - fm.h, cell_pad / 2, br, lad, km, &fb);
        r->title = lv_label_create(r->obj);
        lv_obj_set_style_text_font(r->title, fm.font ? fm.font : jost(g_title_px), 0);
        lv_obj_set_style_text_align(r->title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(r->title, fm.space, 0);
        lv_label_set_long_mode(r->title, fm.whole ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(r->title, fm.text[0] ? fm.text : main);
        int th = fm.h > 0 ? fm.h : lv_font_get_line_height(jost(g_title_px));
        if(th > room) th = room;
        int tail_h = show_tail ? fb.h + cell_pad / 4 : 0;
        int block = th + tail_h + cell_pad / 2 + ah, y0 = (h - block) / 2;
        if(y0 < cell_pad / 2) y0 = cell_pad / 2;
        lv_obj_set_size(r->title, inner_w, th);
        lv_obj_set_pos(r->title, cell_pad, y0);
        if(show_tail) {
            /* the tail rides in the row's `tile` slot: a smaller label */
            r->tile = lv_label_create(r->obj);
            lv_obj_set_style_text_font(r->tile, fb.font, 0);
            lv_obj_set_style_text_align(r->tile, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_line_space(r->tile, fb.space, 0);
            lv_label_set_long_mode(r->tile, fb.whole ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
            lv_label_set_text(r->tile, fb.text[0] ? fb.text : br);
            lv_obj_set_size(r->tile, inner_w, fb.h);
            lv_obj_set_pos(r->tile, cell_pad, y0 + th + cell_pad / 4);
        }
        r->artist = lv_label_create(r->obj);
        lv_label_set_long_mode(r->artist, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(r->artist, fa, 0);
        lv_obj_set_style_text_align(r->artist, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(r->artist, LV_OPA_80, 0);
        char ar[300];
        snprintf(ar, sizeof(ar), "%s", item_subline(item));
        caps_inplace(ar);
        lv_label_set_text(r->artist, ar);
        lv_obj_set_size(r->artist, inner_w, ah);
        lv_obj_set_pos(r->artist, cell_pad, y0 + th + tail_h + cell_pad / 2);
        /* the playing mark: a disc in the corner */
        r->mark = lv_obj_create(r->obj);
        lv_obj_remove_style_all(r->mark);
        int d = g_artist_px * 6 / 10;
        lv_obj_set_size(r->mark, d, d);
        lv_obj_set_pos(r->mark, cell_w - cell_gap - d - cell_pad / 2, cell_pad / 2);
        lv_obj_set_style_radius(r->mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r->mark, LV_OPA_COVER, 0);
        lv_obj_add_flag(r->mark, LV_OBJ_FLAG_HIDDEN);
        row_style(r);
        return r;
    }

    lv_obj_set_pos(r->obj, 0, row_index(item) * rh);
    lv_obj_set_size(r->obj, col_x + col_w, rh);
    if(items[item].kind == ITEM_HEADER) {
        /* the album's title (and year) in small caps toward the row's foot,
         * a hairline under it: the break before the album's tracks */
        const lib_album_t * a = album_of(item);
        char text[400];
        if(a && a->year) snprintf(text, sizeof(text), "%s \xC2\xB7 %d", a->title, a->year);
        else snprintf(text, sizeof(text), "%s", a ? a->title : "");
        caps_inplace(text);
        const lv_font_t * fh = jost(f_artist_px * 115 / 100);
        int hh = lv_font_get_line_height(fh), x0 = tiles ? PX(16) : col_x;
        r->title = lv_label_create(r->obj);
        lv_label_set_long_mode(r->title, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(r->title, fh, 0);
        lv_obj_set_style_text_opa(r->title, LV_OPA_80, 0);
        lv_label_set_text(r->title, text);
        lv_obj_set_size(r->title, col_x + col_w - x0, hh);
        lv_obj_set_pos(r->title, x0, rh - hh - rh / 5);
        r->mark = lv_obj_create(r->obj);                      /* the rule */
        lv_obj_remove_style_all(r->mark);
        lv_obj_set_style_bg_opa(r->mark, LV_OPA_40, 0);
        lv_obj_set_size(r->mark, col_x + col_w - x0, rh / 60 < 1 ? 1 : rh / 60);
        lv_obj_set_pos(r->mark, x0, rh - rh / 6);
        row_style(r);
        return r;
    }
    if(tiles) {
        r->tile = lv_obj_create(r->obj);
        lv_obj_remove_style_all(r->tile);
        lv_obj_remove_flag(r->tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_shadow_color(r->tile, lv_color_black(), 0);
        lv_obj_set_style_shadow_opa(r->tile, LV_OPA_50, 0);
        lv_obj_set_style_bg_opa(r->tile, LV_OPA_COVER, 0);
        r->img = lv_image_create(r->tile);
        lv_obj_set_size(r->img, LV_PCT(100), LV_PCT(100));
        lv_image_set_inner_align(r->img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_add_flag(r->img, LV_OBJ_FLAG_HIDDEN);
        if(item_has_art(item)) {
            const lib_album_t * a = album_of(item);
            lv_obj_set_style_bg_color(r->tile, lv_color_hex(a && a->colour ? a->colour : 0x333333), 0);
        }
        else {
            tile_style_t st;
            companion_style(item, &st);
            lv_obj_set_style_bg_color(r->tile, lv_color_hex(st.bg), 0);
            lv_obj_set_style_text_color(r->tile, lv_color_hex(st.ink), 0);
            tiletext_t tt;
            const lib_album_t * a = album_of(item);
            tiletext_make(r->tile, tile_side, a ? a->title : item_title(item), "", &st, &tt);
        }
    }

    r->title = lv_label_create(r->obj);
    lv_label_set_long_mode(r->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(r->title, col_x, 0);
    lv_label_set_text(r->title, item_title(item));
    r->artist = lv_label_create(r->obj);
    lv_label_set_long_mode(r->artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(r->artist, jost(f_artist_px), 0);
    char ar[300];
    snprintf(ar, sizeof(ar), "%s", item_subline(item));
    caps_inplace(ar);
    lv_label_set_text(r->artist, ar);
    /* the title sits above the row's middle, the artist below */
    const lv_font_t * ft = jost(f_title_px * GROW_PCT / 100), * fa = jost(f_artist_px);
    int th = lv_font_get_line_height(ft), ah = lv_font_get_line_height(fa);
    int block = th + ah - rh / 24, y0 = (rh - block) / 2;
    lv_obj_set_size(r->title, col_w, th);
    lv_obj_set_pos(r->title, col_x, y0);
    lv_obj_set_size(r->artist, col_w, ah);
    lv_obj_set_pos(r->artist, col_x, y0 + th - rh / 24);
    /* the playing mark: a small disc before the title */
    r->mark = lv_obj_create(r->obj);
    lv_obj_remove_style_all(r->mark);
    int d = f_title_px * 30 / 100;
    lv_obj_set_size(r->mark, d, d);
    lv_obj_set_pos(r->mark, col_x - d - f_title_px * 45 / 100, y0 + (th - d) / 2);
    lv_obj_set_style_radius(r->mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r->mark, LV_OPA_COVER, 0);
    lv_obj_add_flag(r->mark, LV_OBJ_FLAG_HIDDEN);
    row_style(r);
    return r;
}

/* Keep the rows for items a..b, drop the rest. */
static void window_set(int a, int b)
{
    if(a < 0) a = 0;
    if(b > nitems - 1) b = nitems - 1;
    for(int r = 0; r < MAX_ROWS; r++) if(rows[r].item >= 0 && (rows[r].item < a || rows[r].item > b)) row_delete(&rows[r]);
    for(int i = a; i <= b; i++) if(!row_of(i)) row_create(i);
    row_want_art();
}

/* The items whose cells touch the content range y0..y1. */
static void window_range(int y0, int y1, int * lo, int * hi)
{
    if(!gridstyle) {
        int a = y0 / rh - 1, b = y1 / rh + 1;
        *lo = a * ncols;
        *hi = b * ncols + ncols - 1;
        return;
    }
    *lo = nitems; *hi = -1;
    for(int i = 0; i < nitems; i++) {
        if(cell_y[i] + cell_h[i] < y0 || cell_y[i] > y1) continue;
        if(i < *lo) *lo = i;
        if(i > *hi) *hi = i;
    }
}

/* The first item wholly on screen (in the grid: the first whose top is in view). */
static int first_item_on(void)
{
    if(nitems == 0) return 0;
    if(!gridstyle) { int i = ((scroll_target + rh - 1) / rh) * ncols; return i < nitems ? i : nitems - 1; }
    for(int i = 0; i < nitems; i++) if(cell_y[i] >= scroll_target) return i;
    return nitems - 1;
}

/* In the grid: the cell above this one in its column is off the top. */
static bool at_top(int item)
{
    if(!gridstyle) return row_index(item) == (scroll_target + rh - 1) / rh;
    return item - ncols < 0 || cell_y[item - ncols] < scroll_target;
}

static void window_normal(void)
{
    int lo, hi, margin = field_hh() / 2;
    window_range(scroll_y - margin, scroll_y + field_hh() + margin, &lo, &hi);
    window_set(lo, hi);
}

/* ------------------------------------------------------------ jump row */

static void jump_render(void)
{
    if(!show_jump) { lv_obj_add_flag(jump_row, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(jump_row, LV_OBJ_FLAG_HIDDEN);
    int s = sel == -1 ? jump_sec : (sel >= 0 && nitems ? items[sel].section : 0);
    if(s < 0 || s >= nsections) s = 0;
    const char * label = nsections ? sections[s].label : "-";
    lv_label_set_text(jump_label, label);
    bool on = sel == -1;
    uint32_t col = on ? c_sel : c_ink;
    lv_obj_set_style_text_color(jump_label, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(jump_l, lv_color_hex(flash_dir < 0 ? c_play : col), 0);
    lv_obj_set_style_text_color(jump_r, lv_color_hex(flash_dir > 0 ? c_play : col), 0);
    lv_obj_set_style_text_opa(jump_l, on ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_text_opa(jump_r, on ? LV_OPA_COVER : LV_OPA_50, 0);
    const lv_font_t * f = jost(on ? f_title_px * GROW_PCT / 100 : f_title_px);
    lv_obj_set_style_text_font(jump_label, f, 0);
    lv_obj_set_style_text_font(jump_l, f, 0);
    lv_obj_set_style_text_font(jump_r, f, 0);
    lv_obj_update_layout(jump_row);
    int h = lv_font_get_line_height(f);
    int lw = lv_obj_get_width(jump_l), mw = lv_obj_get_width(jump_label), gap = f_title_px / 2;
    int x = gridstyle ? cell_x0 + cell_gap / 2 : col_x;
    lv_obj_set_pos(jump_l, x, (jump_h - h) / 2);
    lv_obj_set_pos(jump_label, x + lw + gap, (jump_h - h) / 2);
    lv_obj_set_pos(jump_r, x + lw + gap + mw + gap, (jump_h - h) / 2);
}

static void flash_cb(lv_timer_t * t)
{
    (void)t;
    flash_dir = 0;
    flash_timer = NULL;
    jump_render();
}

static void jump_flash(int dir)
{
    flash_dir = dir;
    jump_render();
    if(flash_timer) lv_timer_delete(flash_timer);
    flash_timer = lv_timer_create(flash_cb, FLASH_MS, NULL);
    lv_timer_set_repeat_count(flash_timer, 1);
}

/* ------------------------------------------------------------ scroll */

static void scroll_exec(void * var, int32_t v) { (void)var; scroll_y = v; lv_obj_set_y(content, -v); }
static void scroll_done(lv_anim_t * a) { (void)a; window_normal(); }

static void scroll_to(int y, bool anim)
{
    int max = list_height() - field_hh();
    if(max < 0) max = 0;
    if(y > max) y = max;
    if(y < 0) y = 0;
    lv_anim_delete(content, scroll_exec);
    int from = scroll_y;
    scroll_target = y;
    if(!anim || from == y) { scroll_exec(content, y); window_normal(); return; }
    /* the rows of both places exist for the slide */
    int lo, hi;
    window_range((from < y ? from : y) - rh, (from > y ? from : y) + field_hh() + rh, &lo, &hi);
    window_set(lo, hi);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, content);
    lv_anim_set_exec_cb(&a, scroll_exec);
    lv_anim_set_values(&a, from, y);
    lv_anim_set_duration(&a, SCROLL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, scroll_done);
    lv_anim_start(&a);
}

/* Keep the selection in view: a row's top is row_index * rh in the content. */
static void ensure_visible(bool anim)
{
    int grow = tiles ? tile_side * (GROW_PCT - 100) / 100 / 2 + 2 : gridstyle ? cell_gap : 0;     /* the grown tile's overhang, the cell's shadow */
    int top = item_top(sel) - grow, bot = item_top(sel) + item_height(sel) + grow;
    if(sel > 0 && items[sel - 1].kind == ITEM_HEADER) top = item_top(sel - 1);   /* an album's first track brings its title along */
    int y = scroll_target;
    if(top < y) y = top;
    else if(bot > y + field_hh()) y = bot - field_hh();
    scroll_to(y, anim);
}

/* ------------------------------------------------------------ art */

static void art_free(void)
{
    if(art_px) { lv_image_cache_drop(&art_dsc); free(art_px); art_px = NULL; }
    art_item = -1;
    art_src = art_album = -1;
}

/* Whether the picture up is the selected item's: its album's cover, or
 * the folder's own picture. */
static bool art_is_sel(void)
{
    if(!art_px || sel < 0) return false;
    if(items[sel].kind == ITEM_FOLDER) return art_item == sel;
    return items[sel].album >= 0 && art_src == items[sel].src && art_album == items[sel].album;
}

/* The genres a run of tracks carries, the most frequent first, joined
 * with " / " - up to `max` of them that fit `len` (Ian, 13 Oct: "you could
 * get an idea of what's in there"). `album` >= 0: that album's tracks of
 * `src`; else every track row of the list. */
static void top_genres(int src, int album, int max, char * out, size_t len)
{
    typedef struct { char name[48]; int n; } g_t;
    enum { MAXG = 96 };
    static g_t g[MAXG];
    int ng = 0;
    out[0] = 0;
    int count = album >= 0 ? (src >= 0 && src < libs_count() ? libs_get(src)->lib.ntracks : 0) : nitems;
    for(int i = 0; i < count; i++) {
        const lib_track_t * t;
        if(album >= 0) { t = &libs_get(src)->lib.tracks[i]; if(t->album != album) continue; }
        else { t = track_of(i); if(!t) continue; }
        if(!t->genre || !t->genre[0]) continue;
        const char * p = t->genre;
        while(*p) {
            while(*p == ' ' || *p == '/' || *p == ';' || *p == ',') p++;
            const char * e = p;
            while(*e && *e != '/' && *e != ';' && *e != ',') e++;
            const char * q = e;
            while(q > p && q[-1] == ' ') q--;
            if(q > p && q - p < 47) {
                char name[48];
                snprintf(name, sizeof(name), "%.*s", (int)(q - p), p);
                int k = 0;
                while(k < ng && strcasecmp(g[k].name, name) != 0) k++;
                if(k == ng && ng < MAXG) { snprintf(g[ng].name, sizeof(g[ng].name), "%s", name); g[ng].n = 0; ng++; }
                if(k < ng) g[k].n++;
            }
            p = e;
        }
    }
    for(int taken = 0; taken < max; taken++) {
        int best = -1;
        for(int k = 0; k < ng; k++) if(g[k].n > 0 && (best < 0 || g[k].n > g[best].n)) best = k;
        if(best < 0) break;
        size_t o = strlen(out);
        if(o + strlen(g[best].name) + (o ? 3 : 0) >= len) break;
        snprintf(out + o, len - o, "%s%s", o ? " / " : "", g[best].name);
        g[best].n = 0;
    }
}

static void notile_drop(void) { if(notile) { lv_obj_delete(notile); notile = NULL; } }

/* ---- the artists' mosaic (0.20) ---- */

static void mosaic_drop(void)
{
    for(int k = 0; k < MOSAIC_MAX; k++) {
        if(mcell[k].px) { lv_image_cache_drop(&mcell[k].dsc); free(mcell[k].px); mcell[k].px = NULL; }
        mcell[k].obj = mcell[k].img = NULL;
    }
    if(mosaic) { lv_obj_delete(mosaic); mosaic = NULL; }
    mosaic_item = -1;
    mosaic_n = 0;
}

/* The square as the selected artist's albums: n x n cells (n = 1..5, the
 * smallest that holds them all, the first 25 when there are more), a cover
 * where there is one, a small name tile in the artist's companion colours
 * where not, and the spare cells in the section's colours. Covers land as
 * they are decoded. */
static void mosaic_show(int item)
{
    if(!square) return;
    if(mosaic && mosaic_item == item) { lv_obj_remove_flag(mosaic, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(mosaic); return; }
    mosaic_drop();
    const artist_t * ar = artist_of(item);
    if(!ar) return;
    int stage = ui_screen.stage;
    int n = 1;
    while(n < 5 && n * n < ar->nalbums) n++;
    int gap = n > 1 ? (stage / 180 < 2 ? 2 : stage / 180) : 0;
    int cell = (stage - gap * (n - 1)) / n;
    int x0 = (stage - (cell * n + gap * (n - 1))) / 2;
    mosaic = lv_obj_create(square);
    lv_obj_remove_style_all(mosaic);
    lv_obj_remove_flag(mosaic, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(mosaic, 0, 0);
    lv_obj_set_size(mosaic, stage, stage);
    lv_obj_set_style_bg_color(mosaic, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(mosaic, LV_OPA_COVER, 0);
    mosaic_item = item;
    mosaic_n = n * n;
    static thumb_req_t reqs[MOSAIC_MAX];
    int nreq = 0;
    gen++;
    uint32_t label, shell, dark;
    tilestyle_picture_colours(seed, &sections[items[item].section].style, &label, &shell, &dark);
    for(int k = 0; k < n * n; k++) {
        lv_obj_t * c = lv_obj_create(mosaic);
        lv_obj_remove_style_all(c);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(c, x0 + (k % n) * (cell + gap), x0 + (k / n) * (cell + gap));
        lv_obj_set_size(c, cell, cell);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        mcell[k].obj = c;
        if(k >= ar->nalbums) {
            /* a spare cell: the section's colours, turn and turn about */
            lv_obj_set_style_bg_color(c, lv_color_hex((k & 1) ? shell : label), 0);
            continue;
        }
        int src = ar->albums[k].src, album = ar->albums[k].album;
        const lib_album_t * a = &libs_get(src)->lib.albums[album];
        if(a->has_art) {
            lv_obj_set_style_bg_color(c, lv_color_hex(a->colour ? a->colour : 0x333333), 0);
            mcell[k].img = lv_image_create(c);
            lv_obj_set_size(mcell[k].img, cell, cell);
            lv_image_set_inner_align(mcell[k].img, LV_IMAGE_ALIGN_STRETCH);
            lv_obj_add_flag(mcell[k].img, LV_OBJ_FLAG_HIDDEN);
            int si = 0;
            while(si < LIB_THUMB_COUNT - 1 && lib_thumb_px[si] < cell) si++;
            while(si > 0 && !(a->thumb_sizes & (1 << si))) si--;
            reqs[nreq].id = MOSAIC_BASE + k;
            reqs[nreq].gen = gen;
            reqs[nreq].px = cell;
            cache_thumb_path(libs_get(src)->card, a, si, reqs[nreq].path, sizeof(reqs[nreq].path));
            nreq++;
        }
        else {
            /* a name tile in a companion of the artist's colours, keyed by the album */
            tile_style_t st;
            tilestyle_companion(seed + (unsigned)(album + 1) * 7919u + (unsigned)src * 131u, &sections[items[item].section].style, &st);
            lv_obj_set_style_bg_color(c, lv_color_hex(st.bg), 0);
            lv_obj_set_style_text_color(c, lv_color_hex(st.ink), 0);
            tiletext_t tt;
            tiletext_make(c, cell, a->title, n <= 2 ? album_artist_text(a) : "", &st, &tt);
        }
    }
    thumbs_want(reqs, nreq);
    lv_obj_move_foreground(mosaic);
}

static void square_show_art(void)
{
    if(!art_img || !art_px) return;
    lv_image_set_src(art_img, &art_dsc);
    int side = ui_screen.stage, w = (int)art_dsc.header.w, h = (int)art_dsc.header.h;
    int dw, dh;
    if(w <= 0 || h <= 0) { dw = dh = side; }
    else if(w * 10 >= h * 12) { dh = side; dw = (int)((long)w * side / h); if(dw > side * 4) dw = side * 4; }
    else { int m = w > h ? w : h; dw = (int)((long)w * side / m); dh = (int)((long)h * side / m); }
    lv_obj_set_size(art_img, dw, dh);
    lv_obj_set_pos(art_img, dw > side ? 0 : (side - dw) / 2, (side - dh) / 2);
    lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    if(pic_img) lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
    notile_drop();
}

/* The drawn companions: the cassette (the theme's main design) or the
 * record, rendered off the UI thread in the section's colours; the latest
 * job wins (as the album page does it). `key`: -1 the folder's or album's
 * own tape (LIST_FOLDER / LIST_ALBUM), else the album the lines are of. */
typedef struct { int gen, section, key; char design[32], font[64], l1[300], l2[300], l3[300]; bool caps; uint32_t bg, ink; placeholder_tint_t tints[8]; int ntints; } pic_job_t;

static void * pic_thread(void * arg)
{
    pic_job_t * j = arg;
    int w = 0, h = 0;
    uint8_t * px = placeholder_render_design(j->design, j->tints, j->ntints, j->bg, j->ink, j->font[0] ? j->font : NULL, j->caps, j->l1, j->l2, j->l3, &w, &h);
    pthread_mutex_lock(&sq_lock);
    if(px && j->gen == sq_gen) {
        free(sq_res.px); sq_res.px = px; sq_res.w = w; sq_res.h = h; sq_res.gen = j->gen;
        sq_res.section = j->section; sq_res.key = j->key; snprintf(sq_res.design, sizeof(sq_res.design), "%s", j->design); sq_res.ready = true;
    }
    else free(px);
    pthread_mutex_unlock(&sq_lock);
    free(j);
    return NULL;
}

static bool pic_is(const char * design, int key) { return pic_px && pic_key == key && pic_section == cur_section && strcmp(pic_design, design) == 0; }

static void pic_render(const char * design, int key, const char * l1, const char * l2, const char * l3)
{
    if(!tv || nsections == 0 || cur_section < 0 || !placeholder_has_design(design)) return;
    if(pic_is(design, key)) return;
    pic_job_t * j = calloc(1, sizeof(*j));
    if(!j) return;
    pthread_mutex_lock(&sq_lock);
    j->gen = ++sq_gen;
    pthread_mutex_unlock(&sq_lock);
    j->section = cur_section;
    j->key = key;
    snprintf(j->design, sizeof(j->design), "%s", design);
    snprintf(j->l1, sizeof(j->l1), "%.299s", l1 ? l1 : "");
    snprintf(j->l2, sizeof(j->l2), "%.299s", l2 ? l2 : "");
    snprintf(j->l3, sizeof(j->l3), "%.299s", l3 ? l3 : "");
    const tile_style_t * st = &sections[cur_section].style;
    uint32_t label, shell, dark;
    tilestyle_picture_colours(seed, st, &label, &shell, &dark);
    unsigned x = seed ^ (unsigned)(key + 7) * 0x9E3779B9u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    bool black = x % 5 == 0;                            /* coloured vinyl most times, black one in five (Ian, 11 Oct) */
    uint32_t disc = strcmp(design, "record") == 0 ? (black ? dark : shell) : dark;
    placeholder_tint_t tints[8] = { {"background", st->bg}, {"label", label}, {"shell", shell}, {"sleeve", shell},
                                    {"centre", label}, {"disc", disc}, {"groove", disc}, {"spindle", label} };
    memcpy(j->tints, tints, sizeof(tints));
    j->ntints = 8;
    j->bg = st->bg;
    j->ink = st->ink;
    const tile_font_t * f = st->font != TILE_FONT_NONE ? tilestyle_font(st->font) : NULL;
    snprintf(j->font, sizeof(j->font), "%s", f ? f->file : "");
    j->caps = f ? f->caps : false;
    pthread_t th;
    if(pthread_create(&th, NULL, pic_thread, j) != 0) { free(j); return; }
    pthread_detach(th);
}

static void pic_drop(void)
{
    pthread_mutex_lock(&sq_lock);
    sq_gen++;
    free(sq_res.px);
    sq_res.px = NULL;
    sq_res.ready = false;
    pthread_mutex_unlock(&sq_lock);
    if(pic_px) { lv_image_cache_drop(&pic_dsc); free(pic_px); pic_px = NULL; }
    pic_section = -1;
    pic_key = -2;
    pic_design[0] = 0;
}

/* Which drawn companion an album without art gets in this list: the name
 * tile, the cassette or the record - one per album, dealt per visit. */
static const char * companion_design(int item)
{
    const char * opt[3];
    int nopt = 0;
    opt[nopt++] = NULL;
    if(placeholder_has_design(placeholder_main_design())) opt[nopt++] = placeholder_main_design();
    if(placeholder_has_design("record")) opt[nopt++] = "record";
    unsigned x = (seed * 2654435761u) ^ (unsigned)(items[item].album + 1) * 40503u;
    x ^= x >> 13; x *= 0x5BD1E995u; x ^= x >> 15;
    return opt[x % (unsigned)nopt];
}

static void notile_make(const char * title, const char * artist, const tile_style_t * st)
{
    notile_drop();
    notile = lv_obj_create(square);
    lv_obj_remove_style_all(notile);
    lv_obj_set_size(notile, ui_screen.stage, ui_screen.stage);
    lv_obj_set_style_bg_color(notile, lv_color_hex(st->bg), 0);
    lv_obj_set_style_bg_opa(notile, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(notile, lv_color_hex(st->ink), 0);
    tiletext_t tt;
    tiletext_make(notile, ui_screen.stage, title, artist, st, &tt);
}

/* The square shows the selected item: its cover, else a drawn companion -
 * the folder's (album's) own cassette in a mixtape-style list, or the
 * item's album as a name tile, a cassette or a record. */
static void square_update(void)
{
    if(!tv || !square) return;
    if(sel < 0 || nitems == 0) return;
    if(items[sel].kind == ITEM_ARTIST) {
        if(art_img) lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        if(pic_img) lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
        notile_drop();
        mosaic_show(sel);
        return;
    }
    if(mosaic) lv_obj_add_flag(mosaic, LV_OBJ_FLAG_HIDDEN);
    if(item_has_art(sel)) {
        if(art_is_sel()) { square_show_art(); return; }
        /* another cover: the one up stays until the new one lands (no tape
         * in between - Ian, 13 Oct); nothing up yet: the companion meanwhile */
        thumb_req_t req = { .id = GEN_BASE + sel, .gen = ++gen, .px = ui_screen.stage };
        const lib_album_t * a = album_of(sel);
        if(items[sel].kind == ITEM_FOLDER) snprintf(req.path, sizeof(req.path), "%s", items[sel].pic);
        else {
            int si = LIB_THUMB_COUNT - 1;
            while(si > 0 && !(a->thumb_sizes & (1 << si))) si--;
            cache_thumb_path(card_of(sel), a, si, req.path, sizeof(req.path));
        }
        thumbs_want(&req, 1);
        if(art_img && !lv_obj_has_flag(art_img, LV_OBJ_FLAG_HIDDEN)) return;
    }
    if(art_img) lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    const lib_album_t * a = album_of(sel);
    if(kind == LIST_FOLDER || kind == LIST_ALBUM) {
        /* the mixtape: the folder's (album's) name, the track count and
         * the genres most of its tracks carry on the label's three lines */
        char n[16], count[64], genres[64];
        snprintf(n, sizeof(n), "%d", nitems);
        ui_strf(count, sizeof(count), S_LIST_TRACKS_COUNT, "n", n, NULL);
        const char * name = kind == LIST_ALBUM && a ? a->title : base_name(folder);
        if(pic_is(placeholder_main_design(), -1)) { notile_drop(); lv_obj_remove_flag(pic_img, LV_OBJ_FLAG_HIDDEN); return; }
        if(pic_img) lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
        top_genres(-1, -1, 3, genres, 30);
        if(placeholder_has_design(placeholder_main_design())) { pic_render(placeholder_main_design(), -1, name, count, genres); return; }
    }
    tile_style_t st;
    companion_style(sel, &st);
    const char * design = items[sel].kind == ITEM_FOLDER || !a ? NULL : companion_design(sel);
    if(design) {
        if(pic_is(design, items[sel].album)) { notile_drop(); lv_obj_remove_flag(pic_img, LV_OBJ_FLAG_HIDDEN); return; }
        if(pic_img) lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
        char genres[48];
        top_genres(items[sel].src, items[sel].album, 2, genres, 26);
        pic_render(design, items[sel].album, album_artist_text(a), a->title, genres);
        return;
    }
    if(pic_img) lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
    if(a) notile_make(a->title, album_artist_text(a), &st);
    else notile_make(item_title(sel), "", &st);
}

static void set_dsc(lv_image_dsc_t * d, uint8_t * px, int w, int h)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_XRGB8888;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 4;
    d->data_size = (uint32_t)w * (uint32_t)h * 4;
    d->data = px;
}

/* The handheld rows' thumbnails: every row on screen with art. */
static void row_want_art(void)
{
    if(!tiles) return;
    static thumb_req_t reqs[MAX_ROWS];
    int n = 0;
    for(int r = 0; r < MAX_ROWS && n < MAX_ROWS; r++) {
        if(rows[r].item < 0 || rows[r].px || !item_has_art(rows[r].item)) continue;
        int item = rows[r].item;
        reqs[n].id = GEN_BASE + item;
        reqs[n].gen = gen;
        reqs[n].px = tile_side;
        if(items[item].kind == ITEM_FOLDER) snprintf(reqs[n].path, sizeof(reqs[n].path), "%s", items[item].pic);
        else if(items[item].kind == ITEM_ARTIST) { if(!artist_art_path(item, tile_side, reqs[n].path, sizeof(reqs[n].path))) continue; }
        else {
            const lib_album_t * a = album_of(item);
            int si = 0;
            while(si < LIB_THUMB_COUNT - 1 && lib_thumb_px[si] < tile_side) si++;
            while(si > 0 && !(a->thumb_sizes & (1 << si))) si--;
            cache_thumb_path(card_of(item), a, si, reqs[n].path, sizeof(reqs[n].path));
        }
        n++;
    }
    thumbs_want(reqs, n);
}

static void poll_cb(lv_timer_t * t)
{
    (void)t;
    pthread_mutex_lock(&sq_lock);
    if(sq_res.ready && pic_img) {
        uint8_t * px = sq_res.px; int w = sq_res.w, h = sq_res.h;
        pic_section = sq_res.section; pic_key = sq_res.key;
        snprintf(pic_design, sizeof(pic_design), "%s", sq_res.design);
        sq_res.px = NULL; sq_res.ready = false;
        pthread_mutex_unlock(&sq_lock);
        if(pic_px) { lv_image_cache_drop(&pic_dsc); free(pic_px); }
        pic_px = px;
        set_dsc(&pic_dsc, px, w, h);
        lv_image_set_src(pic_img, &pic_dsc);
        lv_obj_set_size(pic_img, ui_screen.stage, ui_screen.stage);
        square_update();
    }
    else pthread_mutex_unlock(&sq_lock);
    thumb_res_t r;
    while(thumbs_poll(&r)) {
        if(!r.pixels) continue;
        if(r.id >= MOSAIC_BASE) {
            /* a mosaic cell's cover */
            int k = r.id - MOSAIC_BASE;
            if(r.gen != gen || !mosaic || k < 0 || k >= mosaic_n || !mcell[k].img || mcell[k].px) { free(r.pixels); continue; }
            mcell[k].px = r.pixels;
            set_dsc(&mcell[k].dsc, mcell[k].px, r.w, r.h);
            lv_image_set_src(mcell[k].img, &mcell[k].dsc);
            lv_obj_remove_flag(mcell[k].img, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int item = r.id - GEN_BASE;
        if(item < 0 || item >= nitems) { free(r.pixels); continue; }
        if(tv) {
            if(r.gen != gen || item != sel) { free(r.pixels); continue; }
            art_free();
            art_px = r.pixels;
            art_item = item;
            art_src = items[item].src;
            art_album = items[item].kind == ITEM_FOLDER ? -1 : items[item].album;
            set_dsc(&art_dsc, art_px, r.w, r.h);
            square_show_art();
        }
        else {
            row_t * row = row_of(item);
            if(!row || row->px || !row->img) { free(r.pixels); continue; }
            row->px = r.pixels;
            set_dsc(&row->dsc, row->px, r.w, r.h);
            lv_image_set_src(row->img, &row->dsc);
            lv_obj_remove_flag(row->img, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void settle_cb(lv_timer_t * t)
{
    (void)t;
    settle_timer = NULL;
    square_update();
}

/* ------------------------------------------------------------ selection */

static void restyle_all(void) { for(int r = 0; r < MAX_ROWS; r++) if(rows[r].item >= 0) row_style(&rows[r]); }

static void set_section(int section)
{
    if(section == cur_section) return;
    apply_colours(section);
    restyle_all();
}

static void set_sel_dir(int i, int dir, bool anim);
static void set_sel(int i, bool anim) { set_sel_dir(i, 0, anim); }

/* `dir`: which way to step off an album's break row (+1 down, -1 up); 0 =
 * from where the cursor is. A wrap passes its own (0.20a: the bottom
 * wrapped "up" onto the jump row). */
static void set_sel_dir(int i, int dir, bool anim)
{
    if(nitems == 0) return;
    if(i < -1) i = -1;
    if(i == -1 && !show_jump) i = 0;
    if(i > nitems - 1) i = nitems - 1;
    if(i >= 0 && items[i].kind == ITEM_HEADER) {
        /* never on a header: on in the direction of travel, round the ends */
        if(!dir) dir = i < sel ? -1 : 1;
        int j = i;
        while(j >= 0 && j < nitems && items[j].kind == ITEM_HEADER) j += dir;
        if(j < 0) j = show_jump ? -1 : nitems - 1;
        else if(j >= nitems) { j = 0; while(j < nitems - 1 && items[j].kind == ITEM_HEADER) j++; }
        i = j;
    }
    int old = sel;
    sel = i;
    if(sel >= 0) {
        set_section(items[sel].section);
        ensure_visible(anim);
    }
    else {
        jump_sec = items[first_item_on()].section;
        set_section(jump_sec);
    }
    row_t * ro = row_of(old), * rn = row_of(sel);
    if(ro) row_style(ro);
    if(rn) row_style(rn);
    jump_render();
    if(tv) {
        if(settle_timer) lv_timer_reset(settle_timer);
        else { settle_timer = lv_timer_create(settle_cb, SETTLE_MS, NULL); lv_timer_set_repeat_count(settle_timer, 1); }
    }
}

/* The jump row: to the previous/next stop; the list moves under the row so
 * the stop's first item sits at the top of the field. */
static void jump_step(int dir)
{
    if(nsections < 2 || nitems == 0) return;
    int s = ((jump_sec + dir) % nsections + nsections) % nsections;
    jump_sec = s;
    set_section(s);
    scroll_to(item_top(sections[s].first), true);
    jump_flash(dir);
}

static void page(int dir)
{
    if(nitems == 0) return;
    int step = rows_on - 1;
    if(step < 1) step = 1;
    int target = sel + dir * step * ncols;
    if(target < 0) target = 0;
    if(target > nitems - 1) target = nitems - 1;
    set_sel_dir(target, target == 0 ? +1 : dir, true);
}

static void select_item(void)
{
    if(sel < 0 || nitems == 0) return;
    switch(items[sel].kind) {
        case ITEM_TRACK: {
            const char ** uris = calloc((size_t)nitems, sizeof(char *));
            if(!uris) return;
            int n = 0, start = 0;
            for(int i = 0; i < nitems; i++) if(track_of(i)) { if(i == sel) start = n; uris[n++] = track_of(i)->uri; }
            bool started = playthrough_play_list(uris, n, start);
            free(uris);
            if(started) {
                int was = playing;
                playing = sel;
                row_t * a = row_of(was), * b = row_of(sel);
                if(a) row_style(a);                /* the mark leaves the track played before (Ian, 13 Oct) */
                if(b) row_style(b);
                if(play_cb) play_cb();
            }
            break;
        }
        case ITEM_ALBUM:
            if(open_album_cb) {
                static tile_style_t st;
                companion_style(sel, &st);
                open_album_cb(lib_of(sel), items[sel].album, card_of(sel), item_has_art(sel) ? NULL : &st);
            }
            break;
        case ITEM_FOLDER:
            if(open_folder_cb && folder_of(sel)) open_folder_cb(folder_of(sel)->uri);
            break;
        case ITEM_ARTIST:
            if(open_artist_cb) open_artist_cb(items[sel].artist);
            break;
        case ITEM_HEADER:
            break;
    }
}

/* ------------------------------------------------------------ build */

static void rows_clear(void)
{
    for(int r = 0; r < MAX_ROWS; r++) if(rows[r].item != -2) row_delete(&rows[r]);
    for(int r = 0; r < MAX_ROWS; r++) rows[r].item = -2;
}

static void items_free(void)
{
    for(int i = 0; i < nitems; i++) free(items[i].pic);
    free(items);
    items = NULL;
    nitems = 0;
    cells_free();
}

static void item_add(int src, item_kind_t k, int track, int album, int fo)
{
    item_t * it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->src = src;
    it->kind = k;
    it->track = track;
    it->album = album;
    it->folder = fo;
    it->artist = -1;
}

/* A folder's direct music subfolders as rows (LIST_ALBUMS), as the grid
 * shows them: an album folder is its album, any other a folder row with
 * the folder's own picture and its album count. */
static void items_from_folder(int src, const char * f)
{
    const library_t * lib = &libs_get(src)->lib;
    size_t fl = strlen(f);
    for(int k = 0; k < lib->nfolders; k++) {
        const lib_folder_t * fo = &lib->folders[k];
        if(fl) { if(strncmp(fo->uri, f, fl) != 0 || fo->uri[fl] != '/') continue; }
        else if(!fo->uri[0]) continue;
        const char * rest = fo->uri + (fl ? fl + 1 : 0);
        if(strchr(rest, '/')) continue;
        if(fo->tracks == 0 && fo->subfolders == 0) continue;
        if(mpd_is_container_name(fo->uri)) continue;
        if(fo->type == FOLDER_ALBUM && fo->album >= 0 && fo->album < lib->nalbums) { item_add(src, ITEM_ALBUM, -1, fo->album, -1); continue; }
        item_add(src, ITEM_FOLDER, -1, -1, k);
        item_t * it = &items[nitems - 1];
        char pic[1024];
        if(art_folder_image(fo->uri, pic, sizeof(pic))) it->pic = strdup(pic);
        size_t ul = strlen(fo->uri);
        for(int a = 0; a < lib->nalbums; a++) {
            const lib_album_t * al = &lib->albums[a];
            if(strncmp(al->folder, fo->uri, ul) != 0 || (al->folder[ul] != '/' && al->folder[ul] != 0)) continue;
            it->count++;
            if(al->newest > it->newest) it->newest = al->newest;
        }
    }
}

/* Build the items from the mounted cards' indexes (and, for Recent, the
 * play history). */
static void load(const char * keep_uri)
{
    items_free();
    lib_gen = libs_generation();
    hist_gen = history_generation();
    hist_dirty = false;
    int total = 0;
    for(int s = 0; s < libs_count(); s++) total += kind == LIST_ALBUMS ? libs_get(s)->lib.nfolders : libs_get(s)->lib.ntracks;
    if(kind == LIST_RECENT && history_count() < total) total = history_count();
    if(kind == LIST_ARTISTS || kind == LIST_ARTIST_TRACKS) {
        artists_refresh();
        const artist_t * ar = artists_get(artist_idx);
        total = kind == LIST_ARTISTS ? artists_count() : ar ? ar->ntracks + ar->nalbums : 0;
    }
    items = total ? calloc((size_t)total, sizeof(item_t)) : NULL;
    size_t fl = strlen(folder);
    if(kind == LIST_ARTISTS && items) {
        for(int i = 0; i < artists_count(); i++) {
            const artist_t * ar = artists_get(i);
            item_add(ar->nalbums ? ar->albums[0].src : 0, ITEM_ARTIST, -1, -1, -1);
            items[nitems - 1].artist = i;
            items[nitems - 1].count = ar->nalbums;
            if(ar->pic) items[nitems - 1].pic = strdup(ar->pic);
        }
    }
    else if(kind == LIST_ARTIST_TRACKS && items) {
        const artist_t * ar = artists_get(artist_idx);
        for(int k = 0; ar && k < ar->nalbums; k++) {
            int src = ar->albums[k].src, album = ar->albums[k].album;
            const library_t * lib = &libs_get(src)->lib;
            bool header = sort_kind == SORT_ALBUMS && !gridstyle;   /* a break with the album's title before its tracks (Ian, 14 Oct) */
            if(header && nitems < total) { item_add(src, ITEM_HEADER, -1, album, -1); items[nitems - 1].artist = k; }
            for(int t = 0; t < lib->ntracks && nitems < total; t++) {
                if(lib->tracks[t].album != album) continue;
                item_add(src, ITEM_TRACK, t, album, -1);
                items[nitems - 1].artist = k;              /* the album's place in the artist's list */
            }
        }
    }
    else if(kind == LIST_RECENT && items) {
        for(int h = 0; h < history_count() && nitems < total; h++) {
            const char * uri = history_get(h);
            for(int s = 0; s < libs_count(); s++) {
                const library_t * lib = &libs_get(s)->lib;
                if(strncmp(uri, libs_get(s)->card, strlen(libs_get(s)->card)) != 0) continue;
                for(int t = 0; t < lib->ntracks; t++)
                    if(strcmp(lib->tracks[t].uri, uri) == 0) { item_add(s, ITEM_TRACK, t, lib->tracks[t].album, -1); break; }
            }
        }
    }
    else for(int s = 0; s < libs_count() && items; s++) {
        const library_t * lib = &libs_get(s)->lib;
        if(kind == LIST_FOLDER || kind == LIST_ALBUMS) {
            if(strncmp(folder, libs_get(s)->card, strlen(libs_get(s)->card)) != 0) continue;
            if(kind == LIST_ALBUMS) { items_from_folder(s, folder); continue; }
        }
        if(kind == LIST_ALBUM && s != album_src) continue;
        for(int t = 0; t < lib->ntracks; t++) {
            if(kind == LIST_FOLDER && (strncmp(lib->tracks[t].uri, folder, fl) != 0 || lib->tracks[t].uri[fl] != '/')) continue;
            if(kind == LIST_ALBUM && lib->tracks[t].album != album_idx) continue;
            item_add(s, ITEM_TRACK, t, lib->tracks[t].album, -1);
        }
    }
    if(nitems > 1 && sort_kind != SORT_PLAYED && kind != LIST_ARTISTS) qsort(items, (size_t)nitems, sizeof(item_t), item_cmp);   /* artists come in their own order, Various Artists last */
    build_sections();
    sel = nitems ? 0 : -1;
    if(keep_uri) for(int i = 0; i < nitems; i++) if(items[i].kind != ITEM_HEADER && strcmp(item_uri(i), keep_uri) == 0) { sel = i; break; }
    while(sel >= 0 && sel < nitems - 1 && items[sel].kind == ITEM_HEADER) sel++;
    fprintf(stderr, "simpleton-ui: list: %s%s%s: %d rows, %s, %d stops\n", view_id(), folder[0] ? " " : "", folder,
            nitems, sort_kind == SORT_FILE ? "file order" : sort_kind == SORT_PLAYED ? "last played" : sort_kind == SORT_ALBUMS ? "album order" : library_sort_name((lib_sort_t)sort_kind), nsections);
}

static char * keep_key(void)
{
    return (nitems && sel >= 0) ? strdup(item_uri(sel)) : NULL;
}

static void find_playing(void)
{
    playing = -1;
    mpd_status_t st;
    if(!mpd_status(&st) || !st.file[0]) return;
    for(int i = 0; i < nitems; i++) if(track_of(i) && strcmp(track_of(i)->uri, st.file) == 0) { playing = i; break; }
}

static void layout_screen(void)
{
    mosaic_drop();
    if(square) { lv_obj_delete(square); square = NULL; art_img = pic_img = notile = NULL; }
    fx = 0;
    if(tv) {
        int stage = ui_screen.stage, rest = ui_screen.w - stage;
        bool left = (seed >> 3) & 1;
        int ax = left ? 0 : rest;
        fx = left ? stage : 0;
        fw = rest;
        square = lv_obj_create(scr);
        lv_obj_remove_style_all(square);
        lv_obj_remove_flag(square, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(square, ax, ui_screen.stage_y);
        lv_obj_set_size(square, stage, stage);
        lv_obj_set_style_bg_color(square, lv_color_hex(c_bg), 0);
        lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
        pic_img = lv_image_create(square);
        lv_obj_set_size(pic_img, stage, stage);
        lv_image_set_inner_align(pic_img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_add_flag(pic_img, LV_OBJ_FLAG_HIDDEN);
        if(pic_px) lv_image_set_src(pic_img, &pic_dsc);
        art_img = lv_image_create(square);
        lv_obj_set_size(art_img, stage, stage);
        lv_image_set_inner_align(art_img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        if(art_px) square_show_art();
    }
    else { fx = ui_screen.stage_x; fw = ui_screen.stage; }
    lv_obj_set_pos(jump_row, fx, top_y);
    lv_obj_set_size(jump_row, fw, jump_h ? jump_h : 1);
    lv_obj_set_pos(field, fx, top_y + jump_h);
    lv_obj_set_size(field, fw, field_hh());
    lv_obj_set_size(content, fw, list_height() > 0 ? list_height() : 1);
    lv_obj_set_style_text_font(msg, ui_font_px(f_title_px), 0);
    lv_obj_center(msg);
}

static int empty_string(void)
{
    if(!libs_loaded()) return S_BROWSER_STARTING;
    switch(kind) {
        case LIST_RECENT: return S_LIST_RECENT_EMPTY;
        case LIST_ALBUMS: return S_LIST_ALBUMS_EMPTY;
        case LIST_ARTISTS: return S_ARTISTS_EMPTY;
        default: return S_LIST_EMPTY;
    }
}

static void rebuild(void)
{
    rows_clear();
    measure();
    build_sections();
    measure();                                     /* show_jump depends on the stops */
    deal_sections();
    if(nitems == 0) {
        cur_section = -1;
        apply_colours(0);
        layout_screen();
        lv_label_set_text(msg, T(empty_string()));
        lv_obj_set_style_text_color(msg, lv_color_hex(c_ink), 0);
        lv_obj_remove_flag(msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(jump_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    if(sel > nitems - 1) sel = nitems - 1;
    if(sel < 0) sel = 0;
    cur_section = -1;
    apply_colours(items[sel].section);
    cells_free();
    if(gridstyle) {
        /* every cell its own colours and face, dealt as the tiles are: no
         * two neighbours alike, a screenful before a colour repeats */
        cell_st = calloc((size_t)nitems, sizeof(tile_style_t));
        bool * noart = calloc((size_t)nitems, sizeof(bool));
        int * letters = calloc((size_t)nitems, sizeof(int));
        if(cell_st && noart && letters) {
            for(int i = 0; i < nitems; i++) { noart[i] = true; letters[i] = count_letters(item_title(i)); }
            tilestyle_deal(seed, nitems, GRID_COLS, noart, letters, cell_st);
        }
        free(noart); free(letters);
        cell_layout();
    }
    layout_screen();
    scroll_y = scroll_target = 0;
    lv_obj_set_y(content, 0);
    {
        int y = item_top(sel) - (field_hh() - item_height(sel)) / 2;
        int max = list_height() - field_hh();
        if(y > max) y = max;
        if(y < 0) y = 0;
        scroll_target = y;
        scroll_exec(content, y);
    }
    window_normal();
    restyle_all();
    jump_render();
    ui_list_t plm;
    ui_list_metrics(UI_SIZE_DEFAULT, &plm);
    plm.x = fx; plm.y = top_y + jump_h; plm.w = fw; plm.h = field_hh();
    picker_relayout(&plm);
    square_update();
}

/* ------------------------------------------------------------ picker */

static void on_jump(int label, void * ctx)
{
    (void)ctx;
    if(label < 0 || label >= nsections) return;
    set_sel_dir(sections[label].first, +1, true);          /* a stop starts with its album's break: land on the first track */
}

static void on_change(const picker_opts_t * o, void * ctx)
{
    (void)ctx;
    bool restyle = o->style != view_opts.style, resize = o->size != view_opts.size, resort = o->sort != view_opts.sort;
    view_opts.style = o->style;
    view_opts.size = o->size;
    view_opts.strip = o->strip;
    view_opts.sort = o->sort;
    if(kind == LIST_ARTISTS) { if(opens != o->sort) resize = true; opens = o->sort; resort = false; }   /* the rows' counts follow "Opens as": redraw */
    if(resort) sort_override = -1;                 /* the picker's choice stands from here */
    write_options();
    if(restyle && style_ids[view_opts.style] == STYLE_COVERS) { if(style_cb) style_cb("grid"); return; }
    if(restyle && style_ids[view_opts.style] == STYLE_ROWS) { if(style_cb) style_cb("rows"); return; }
    if(resort) {
        sort_kind = sort_ids[view_opts.sort];
        view_opts.jump_row = sort_kind != SORT_FILE && sort_kind != SORT_PLAYED && sort_kind != LIB_SORT_NEWEST;
            char * keep = keep_key();
        load(keep);
        free(keep);
    }
    if(restyle) {
        tiles = !tv && style_ids[view_opts.style] == STYLE_TILES; gridstyle = style_ids[view_opts.style] == STYLE_GRID;
        if(kind == LIST_ARTIST_TRACKS && !resort) { char * keep = keep_key(); load(keep); free(keep); }   /* the album breaks come and go with the grid */
    }
    if(restyle || resize || resort) rebuild();
}

static void on_close(bool back, void * ctx)
{
    (void)ctx;
    (void)back;
    mark_visited();
    restyle_all();
}

static void open_full(void)
{
    view_opts.jump = (sel >= 0 && nitems) ? items[sel].section : (sel == -1 ? jump_sec : 0);
    visited_before_open = visited;
    picker_open(PICKER_FULL, &view_opts, on_jump, on_change, on_close, NULL);
}

static void after_enter(void)
{
    if(suppress_overlay) { suppress_overlay = false; return; }
    if(nitems == 0) return;
    if(!visited) open_full();
}

/* ------------------------------------------------------------ public */

static void reload(bool fresh)
{
    char * keep = fresh ? NULL : keep_key();
    load(keep);
    free(keep);
    find_playing();
    rebuild();
}

static void index_timer_cb(lv_timer_t * t)
{
    (void)t;
    if(!shown) return;
    if(libs_refresh() || lib_gen != libs_generation()) reload(false);
    else if(kind == LIST_RECENT && hist_dirty && nitems == 0) reload(true);
}

void listpage_create(void (*on_exit)(void), void (*on_play)(void), void (*on_style)(const char * style))
{
    exit_cb = on_exit;
    play_cb = on_play;
    style_cb = on_style;
    for(int r = 0; r < MAX_ROWS; r++) rows[r].item = -2;
    thumbs_init();
    char theme_path[512];
    config_theme_file(theme_path, sizeof(theme_path));
    tilestyle_load(theme_path);

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    field = lv_obj_create(scr);
    lv_obj_remove_style_all(field);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    content = lv_obj_create(field);
    lv_obj_remove_style_all(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    jump_row = lv_obj_create(scr);
    lv_obj_remove_style_all(jump_row);
    lv_obj_remove_flag(jump_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(jump_row, LV_OPA_COVER, 0);
    lv_obj_add_flag(jump_row, LV_OBJ_FLAG_HIDDEN);
    jump_l = lv_label_create(jump_row);
    jump_label = lv_label_create(jump_row);
    jump_r = lv_label_create(jump_row);
    lv_label_set_text(jump_l, "<<");
    lv_label_set_text(jump_r, ">>");
    lv_label_set_text(jump_label, "");
    msg = lv_label_create(scr);
    lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
    poll_timer = lv_timer_create(poll_cb, 40, NULL);
    index_timer = lv_timer_create(index_timer_cb, 3000, NULL);
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
    playing = -1;
    sel = -1;
}

void listpage_set_callbacks(void (*open_album)(const library_t *, int, const char *, const tile_style_t *), void (*open_folder)(const char *))
{
    open_album_cb = open_album;
    open_folder_cb = open_folder;
}

void listpage_set_artist_callback(void (*open_artist)(int)) { open_artist_cb = open_artist; }

static void reset_for(list_kind_t k, const char * f)
{
    bool same = k == kind && strcmp(f ? f : "", folder) == 0;
    kind = k;
    snprintf(folder, sizeof(folder), "%s", f ? f : "");
    if(!same) { items_free(); sections_free(); sel = -1; lib_gen = 0; pic_drop(); art_free(); }
}

bool listpage_open(list_kind_t k, const char * f)
{
    if(k == LIST_FOLDER || k == LIST_ALBUMS) {
        if(!f || !f[0]) return false;
        libs_refresh();
        const lib_folder_t * fo;
        if(!libs_find_folder(f, &fo)) return false;
    }
    else f = NULL;
    if(k == LIST_ALBUM || k == LIST_ARTIST_TRACKS) return false;    /* listpage_open_album() / listpage_open_artist_tracks() */
    album_src = album_idx = -1;
    artist_idx = -1;
    sort_override = -1;
    reset_for(k, f);
    return true;
}

bool listpage_open_artist_tracks(int artist, bool by_album)
{
    artists_refresh();
    const artist_t * ar = artists_get(artist);
    if(!ar) return false;
    bool same = kind == LIST_ARTIST_TRACKS && artist == artist_idx;
    album_src = album_idx = -1;
    artist_idx = artist;
    sort_override = by_album ? SORT_ALBUMS : LIB_SORT_TITLE;
    kind = LIST_ARTIST_TRACKS;
    snprintf(folder, sizeof(folder), "%s", ar->name);
    if(!same) { items_free(); sections_free(); sel = -1; lib_gen = 0; pic_drop(); art_free(); }
    return true;
}

int listpage_artist(void) { return kind == LIST_ARTIST_TRACKS ? artist_idx : -1; }

bool listpage_open_album(const char * card, int album)
{
    libs_refresh();
    int src = -1;
    for(int s = 0; s < libs_count(); s++) if(strcmp(libs_get(s)->card, card) == 0) { src = s; break; }
    if(src < 0 || album < 0 || album >= libs_get(src)->lib.nalbums) return false;
    bool same = kind == LIST_ALBUM && src == album_src && album == album_idx;
    album_src = src;
    album_idx = album;
    snprintf(album_card, sizeof(album_card), "%s", card);
    kind = LIST_ALBUM;
    snprintf(folder, sizeof(folder), "%s", libs_get(src)->lib.albums[album].folder);
    if(!same) { items_free(); sections_free(); sel = -1; lib_gen = 0; pic_drop(); art_free(); }
    return true;
}

list_kind_t  listpage_kind(void) { return kind; }

bool listpage_focus_key(const char * key)
{
    if(!key) return false;
    for(int i = 0; i < nitems; i++) if(strcmp(item_uri(i), key) == 0) { if(shown) set_sel(i, false); else sel = i; return true; }
    return false;
}
const char * listpage_folder(void) { return (kind == LIST_FOLDER || kind == LIST_ALBUMS || kind == LIST_ALBUM) ? folder : NULL; }

void listpage_show(bool fresh)
{
    static unsigned seeds[LIST_ARTIST_TRACKS + 1];     /* each kind's last deal: coming back wears the same colours (0.20) */
    shown = true;
    tv = ui_screen.kind == UI_SCREEN_TV;
    if(fresh) seed = (unsigned)time(NULL) ^ (unsigned)lv_tick_get() ^ (unsigned)rand();
    else if(seeds[kind]) seed = seeds[kind];
    seeds[kind] = seed;
    suppress_overlay = !fresh;
    read_options();
    measure();
    lv_screen_load(scr);
    ui_list_t plm;
    ui_list_metrics(UI_SIZE_DEFAULT, &plm);
    picker_attach(scr, &plm);
    lv_timer_resume(poll_timer);
    lv_timer_resume(index_timer);
    if(kind == LIST_ALBUM && (album_src >= libs_count() || strcmp(libs_get(album_src)->card, album_card) != 0)) {
        /* the cards changed under us: find the album again */
        int src = -1;
        for(int s = 0; s < libs_count(); s++) if(strcmp(libs_get(s)->card, album_card) == 0) { src = s; break; }
        album_src = src;
        if(src < 0 || album_idx >= libs_get(src)->lib.nalbums) album_idx = -1;
    }
    if(kind == LIST_ARTIST_TRACKS && (artists_refresh() || !artists_get(artist_idx))) {
        /* the cards changed under us: find the artist again by name */
        int i = artists_find(folder);
        artist_idx = i;
        lib_gen = 0;
    }
    if(libs_refresh() || lib_gen != libs_generation() || !items || (kind == LIST_RECENT && (hist_dirty || hist_gen != history_generation()))) {
        char * keep = fresh ? NULL : keep_key();
        load(keep);
        free(keep);
    }
    if(fresh && kind != LIST_TRACKS) sel = nitems ? 0 : -1;    /* Tracks comes back where it was left (Ian, 23:53), as Albums does */
    while(sel >= 0 && sel < nitems - 1 && items[sel].kind == ITEM_HEADER) sel++;
    if(sel < 0 && nitems) sel = 0;
    find_playing();
    rebuild();
    after_enter();
}

void listpage_hide(void)
{
    shown = false;
    if(picker_mode() != PICKER_NONE) picker_close();
    if(settle_timer) { lv_timer_delete(settle_timer); settle_timer = NULL; }
    if(flash_timer) { lv_timer_delete(flash_timer); flash_timer = NULL; flash_dir = 0; }
    lv_timer_pause(poll_timer);
    lv_timer_pause(index_timer);
    thumbs_want(NULL, 0);
    rows_clear();
    pic_drop();
    art_free();
    notile_drop();
    mosaic_drop();
}

bool listpage_overlay_active(void) { return shown && picker_mode() != PICKER_NONE; }

void listpage_toggle_picker(void)
{
    if(!shown) return;
    if(picker_mode() != PICKER_NONE) { picker_toggle_panel(); return; }
    open_full();
}

void listpage_show_picker_style(void)
{
    if(!shown || nitems == 0) return;
    if(picker_mode() != PICKER_FULL) { if(picker_mode() != PICKER_NONE) picker_close(); open_full(); }
    picker_focus_style();
}

void listpage_reload_options(void)
{
    int old_style = view_opts.style, old_sort = view_opts.sort;
    ui_size_t old_size = view_opts.size;
    read_options();
    if(!shown) return;
    if(view_opts.sort != old_sort) { char * keep = keep_key(); load(keep); free(keep); }
    if(view_opts.style != old_style || view_opts.size != old_size || view_opts.sort != old_sort) rebuild();
}

void listpage_library_changed(void)
{
    if(shown) lv_timer_ready(index_timer);
}

void listpage_history_changed(void)
{
    /* Recent reorders on the next visit, not under the cursor */
    hist_dirty = true;
    if(shown && kind == LIST_RECENT && nitems == 0) lv_timer_ready(index_timer);
}

void listpage_mpd_changed(void)
{
    if(!shown) return;
    int was = playing;
    find_playing();
    if(was != playing) {
        row_t * a = row_of(was), * b = row_of(playing);
        if(a) row_style(a);
        if(b) row_style(b);
    }
}

/* The cell next door in the other column (Ian, 23:53): going right, the
 * adjacent cell at or below this one's top; going left, the one at or
 * above it - the columns rarely line up exactly. */
static int across(int item, int dir)
{
    int col = col_index(item) + dir;
    if(col < 0 || col >= ncols || !cell_y) return item + dir;
    int best = -1;
    for(int j = col; j < nitems; j += ncols) {
        if(dir > 0) { if(cell_y[j] >= cell_y[item]) { best = j; break; } best = j; }
        else { if(cell_y[j] > cell_y[item]) break; best = j; }
    }
    return best >= 0 ? best : item + dir;
}

/* How many items a screen holds: a page's worth of Left/Right in the grid. */
static int per_screen(void)
{
    if(!gridstyle) return rows_on * ncols;
    int lo, hi;
    window_range(scroll_target, scroll_target + field_hh(), &lo, &hi);
    int n = hi >= lo ? hi - lo + 1 : ncols;
    return n < ncols ? ncols : n;
}

void listpage_handle_action(ui_action_t a)
{
    /* Up from the jump row goes on up the list (Ian, 14 Oct: it had wrapped
     * to the bottom); while Up is held to a run, the jump row isn't stopped
     * at again, so the list scrolls on */
    static bool up_run;
    if(picker_mode() != PICKER_NONE) { picker_handle_action(a); return; }
    if(nitems == 0) { if(a == ACT_BACK && exit_cb) exit_cb(); return; }
    int c = ncols;
    if(a != ACT_UP) up_run = false;
    switch(a) {
        case ACT_UP:
            if(sel == -1) {
                int t = first_item_on() - (gridstyle ? 1 : c);
                while(t >= 0 && items[t].kind == ITEM_HEADER) t--;
                if(t < 0) t = nitems - 1;                                              /* already at the top: round to the bottom, as any Up there */
                up_run = true;
                set_sel(t, true);
            }
            else if(show_jump && at_top(sel) && !up_run) set_sel(-1, true);           /* the top row on screen: the jump row */
            else if(sel - c < 0) {
                /* the first row wraps to the last, same column */
                int e = (rows_total() - 1) * c + col_index(sel);
                set_sel(e < nitems ? e : nitems - 1, true);
            }
            else set_sel(sel - c, true);
            break;
        case ACT_DOWN:
            if(sel == -1) set_sel(first_item_on(), true);                             /* into the row under the jump row */
            else if(sel + c < nitems) set_sel(sel + c, true);
            else if(row_index(sel) < rows_total() - 1) set_sel(nitems - 1, true);
            else set_sel_dir(col_index(sel), +1, true);                              /* the bottom wraps to the top */
            break;
        case ACT_LEFT:
            if(sel == -1) jump_step(-1);
            else if(c == 1) page(-1);
            else if(col_index(sel) > 0) set_sel(across(sel, -1), true);
            else if(sel == 0) set_sel(nitems - 1, true);
            else { int t = sel - per_screen(); set_sel(t < 0 ? col_index(sel) : t, true); }   /* off the left edge: a page back */
            break;
        case ACT_RIGHT:
            if(sel == -1) jump_step(+1);
            else if(c == 1) page(+1);
            else if(col_index(sel) < c - 1 && sel + 1 < nitems) set_sel(across(sel, +1), true);
            else if(sel + 1 >= nitems) set_sel_dir(0, +1, true);
            else { int t = sel + per_screen(); set_sel(t > nitems - 1 ? nitems - 1 : t, true); }
            break;
        case ACT_SELECT:
            if(sel == -1) jump_step(+1); else select_item();
            break;
        case ACT_BACK:
            if(exit_cb) exit_cb();
            break;
        default: break;
    }
}
