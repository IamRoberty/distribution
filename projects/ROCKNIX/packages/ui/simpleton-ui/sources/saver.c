/*
 * SimpletonOS UI - the TV's screensavers (implementation). See saver.h.
 *
 * Everything lives on LVGL's top layer in one full-screen box, hidden
 * until started. Art drift and Palettes (Ian, 18 Oct 15:24) work on a
 * grid: the width in 12 columns, a 2:1 stage of 12 x 6 cells between
 * black bars top and bottom (1920 x 1080: 160 px cells, 60 px bars). Every
 * five seconds a piece - a cover from the cache, decoded by the thumbs
 * worker, or a square in one of the set's colours - lands on the grid, 1,
 * 2, 3, 4 or 6 cells square, and stays: pieces pile up, nothing fades
 * away. A piece is safe from being covered for its first 20 s; after that
 * new pieces may overlap it, and one with no cell left on top is deleted.
 * The colour scheme moves on every minute or two and the piling squares
 * carry the change. The cover screensaver asks MPD what plays every two
 * seconds and swaps the picture on a change.
 */
#include "saver.h"
#include "cache.h"
#include "layout.h"
#include "libs.h"
#include "mpdc.h"
#include "palette.h"
#include "strings.h"
#include "theme.h"
#include "thumbs.h"
#include "tilestyle.h"
#include "tiletext.h"
#include "viz.h"
#include "classic.h"
#include "vizmenu.h"
#include "config.h"
#include "dst.h"
#include "objmenu.h"
#include "nowplaying.h"
#include "power.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIECES_MAX     96        /* far more than the 72 cells can show  */
#define TICK_MS        250
#define SPAWN_MS       5000      /* a new piece this often (drift)       */
#define FADE_IN_MS     900       /* a piece's arrival                    */
#define PROTECT_MS     20000     /* nothing lands on a piece this young  */
#define SET_MIN_MS     60000     /* the colour scheme moves on after 1-2 min */
#define SET_MAX_MS     120000
#define GROUND_FADE_MS 4000
#define COVER_POLL_MS  2000
#define COVER_PAN_MS   40000
#define COLS           12
#define ROWS           6
#define BALL_FRAME_MS  33        /* bounce: the cover's frame rate      */
#define BALL_PCT       44        /* bounce: the cover, % of the height (480 at 1080p) */
#define BALL_SPEED_PCT 9         /* bounce: % of the width per second   */
#define BOOM_MS        2700      /* bounce: the corner's surprise       */
#define BOOM_SWEEP_MS  1800      /* ...a ring crosses the screen in this */
#define BOOM_FLASH_MS  110
#define BOOM_RINGS     9         /* a ring is a ninth of the height wide */
#define BOOM_AFTER_MS  1200      /* ...then the whole screen flashes, 200 ms a colour */
#define BOOM_REST_MS   5000      /* bounce: from the rings' end to the cover's return */
#define CORNER_EVERY   1000      /* bounce: the corner every N bounces (SIMPLETON_BOUNCE_EVERY overrides) */

typedef struct {
    lv_obj_t *     obj;
    uint8_t *      pixels;       /* an image piece owns its decode */
    lv_image_dsc_t dsc;
    uint32_t       born_ms;      /* when it landed (an image: when its decode did) */
    int            col, row, size;   /* its cells (drift / palettes); size 0 = off the grid (cover) */
    bool           solid;        /* fully arrived: may hide what is under it */
    int            thumb_id;     /* the request this piece waits for, -1 = none */
    thumb_req_t    req;          /* ...and the request itself (the worker's list is rebuilt from these) */
} piece_t;

static lv_obj_t * root, * title_lbl, * artist_lbl;
static piece_t * pieces[PIECES_MAX];      /* allocated one by one: the image keeps a pointer to its dsc */
static int npieces;
static saver_kind_t kind;
static bool manual;                         /* started from the Visualization page */
static lv_timer_t * timer;
static uint32_t last_spawn_ms, next_set_ms, last_poll_ms;
static uint32_t ground_from, ground_to, ground_now;
static uint32_t ground_started_ms;
static int      palette_set = -1;           /* the set the squares come from */
static int      req_gen;                    /* thumbs results carry it */
static unsigned rnd_state;

/* the albums with cached thumbnails, dealt without repeats */
typedef struct { int src, album; } deck_card_t;
static deck_card_t * deck;
static int deck_n, deck_pos;

/* the cover screensaver's current album */
static char cover_file[1024];
static int cover_src = -1, cover_album = -1;

static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static int rnd_between(int a, int b) { return b > a ? a + (int)(rnd() % (unsigned)(b - a + 1)) : a; }

static uint32_t mix(uint32_t a, uint32_t b, int pct_b) { return ui_mix(a, b, pct_b); }

/* ------------------------------------------------------------ ground */

static uint32_t darken(uint32_t c, int pct) { return mix(c, 0x000000, pct); }

/* Move on to another of the theme's colour sets (the squares follow). */
static void pick_set(void)
{
    int n = tilestyle_set_count();
    if(n <= 0) { palette_set = -1; return; }
    int set = (int)(rnd() % (unsigned)n);
    if(n > 1 && set == palette_set) set = (set + 1) % n;
    palette_set = set;
}

static uint32_t set_colour(void)
{
    uint32_t cols[16];
    int c = palette_set >= 0 ? tilestyle_set(palette_set, cols, 16) : 0;
    if(c <= 0) return 0x1A1612;
    return cols[rnd() % (unsigned)c];
}

static uint32_t pick_ground(void)
{
    pick_set();
    return darken(set_colour(), 35);       /* the ground sits back, the pieces come forward */
}

/* The grid: 12 columns across the width, a 2:1 stage of 12 x 6 cells
 * between black bars (1920 x 1080: 160 px cells, 60 px bars). */
static int cell_x(int c) { return c * ui_screen.w / COLS; }
static int stage_h(void) { return ui_screen.w / 2 < ui_screen.h ? ui_screen.w / 2 : ui_screen.h; }
static int bar_h(void) { return (ui_screen.h - stage_h()) / 2; }
static int cell_y(int r) { return bar_h() + r * stage_h() / ROWS; }
static lv_obj_t * stage;                    /* the ground between the bars */

static void ground_set(uint32_t to, bool at_once)
{
    ground_from = at_once ? to : ground_now;
    ground_to = to;
    ground_started_ms = lv_tick_get();
    if(at_once) { ground_now = to; lv_obj_set_style_bg_color(stage, lv_color_hex(to), 0); }
}

static void ground_tick(uint32_t now)
{
    if(ground_now == ground_to) return;
    uint32_t t = now - ground_started_ms;
    int pct = t >= GROUND_FADE_MS ? 100 : (int)(t * 100 / GROUND_FADE_MS);
    ground_now = pct >= 100 ? ground_to : mix(ground_from, ground_to, pct);
    lv_obj_set_style_bg_color(stage, lv_color_hex(ground_now), 0);
}

/* ------------------------------------------------------------ pieces */

static void piece_free(piece_t * p)
{
    if(p->obj) lv_obj_delete(p->obj);
    free(p->pixels);
    free(p);
}

static piece_t * ball;                      /* bounce: the cover on the move */
static bool ball_wants_palette;             /* bounce: the index had no colours - read them off the decode */
static void ball_landed(const uint8_t * xrgb, int w, int h);

/* drop piece i (order doesn't matter) */
static void piece_drop(int i)
{
    if(pieces[i] == ball) ball = NULL;
    piece_free(pieces[i]);
    pieces[i] = pieces[--npieces];
}

static void pieces_clear(void)
{
    while(npieces) piece_drop(0);
}

static void opa_exec(void * obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }

static void fade(lv_obj_t * obj, int from, int to, uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_start(&a);
}

/* A size for a new piece, in cells: 1, 2, 3, 4 or 6 (Ian: the twelve
 * divides into halves, thirds, quarters and sixths). Small ones often. */
static int pick_size(void)
{
    unsigned r = rnd() % 100;
    return r < 30 ? 1 : r < 60 ? 2 : r < 80 ? 3 : r < 93 ? 4 : 6;
}

/* A spot on the grid for a piece of `size` cells: whole, inside the stage,
 * and not over a piece younger than PROTECT_MS (one still waiting for its
 * decode counts as young). A few tries at the size asked, then smaller;
 * false when the grid has no room this time (the piece waits). */
static bool place(int * size, int * col, int * row)
{
    uint32_t now = lv_tick_get();
    for(int sz = *size; sz >= 1; sz = sz == 6 ? 4 : sz - 1) {
        for(int tries = 0; tries < 24; tries++) {
            int c = rnd_between(0, COLS - sz), r = rnd_between(0, ROWS - sz);
            bool clear = true;
            for(int i = 0; i < npieces && clear; i++) {
                piece_t * p = pieces[i];
                if(p->size <= 0) continue;
                if(p->thumb_id < 0 && now - p->born_ms >= PROTECT_MS) continue;
                if(c < p->col + p->size && c + sz > p->col && r < p->row + p->size && r + sz > p->row) clear = false;
            }
            if(clear) { *size = sz; *col = c; *row = r; return true; }
        }
    }
    return false;
}

/* After a piece has fully arrived: anything it leaves no cell of on top
 * goes (the pile only ever shows its surface). Oldest first, each solid
 * piece stamps its cells; a piece with no stamp left is hidden. */
static void drop_hidden(void)
{
    if(npieces < 2) return;
    int order[PIECES_MAX];
    for(int i = 0; i < npieces; i++) order[i] = i;
    for(int i = 1; i < npieces; i++) {                     /* insertion sort by age */
        int k = order[i], j = i;
        while(j > 0 && (int32_t)(pieces[order[j - 1]]->born_ms - pieces[k]->born_ms) > 0) { order[j] = order[j - 1]; j--; }
        order[j] = k;
    }
    int top[ROWS][COLS];
    for(int r = 0; r < ROWS; r++) for(int c = 0; c < COLS; c++) top[r][c] = -1;
    for(int n = 0; n < npieces; n++) {
        piece_t * p = pieces[order[n]];
        if(p->size <= 0 || !p->solid) continue;
        for(int r = p->row; r < p->row + p->size; r++) for(int c = p->col; c < p->col + p->size; c++) top[r][c] = order[n];
    }
    bool seen[PIECES_MAX] = { false };
    for(int r = 0; r < ROWS; r++) for(int c = 0; c < COLS; c++) if(top[r][c] >= 0) seen[top[r][c]] = true;
    for(int i = 0; i < npieces; ) {
        piece_t * p = pieces[i];
        if(p->size > 0 && p->solid && !seen[i]) { piece_drop(i); seen[i] = seen[npieces]; continue; }
        i++;
    }
}

static piece_t * piece_new(void)
{
    if(npieces >= PIECES_MAX) {
        /* the oldest goes early */
        int oldest = 0;
        for(int i = 1; i < npieces; i++) if(pieces[i]->born_ms < pieces[oldest]->born_ms) oldest = i;
        piece_drop(oldest);
    }
    piece_t * p = calloc(1, sizeof(*p));
    if(!p) return NULL;
    pieces[npieces++] = p;
    p->thumb_id = -1;
    p->born_ms = lv_tick_get();
    return p;
}

/* The piece's box on the grid. */
static void piece_place(piece_t * p, int size, int col, int row)
{
    p->size = size; p->col = col; p->row = row;
    int x = cell_x(col), y = cell_y(row);
    lv_obj_set_pos(p->obj, x, y);
    lv_obj_set_size(p->obj, cell_x(col + size) - x, cell_y(row + size) - y);
}

/* A square of one of the set's colours on the grid. */
static void spawn_square(void)
{
    if(palette_set < 0) return;
    int size = pick_size(), col, row;
    if(!place(&size, &col, &row)) return;
    piece_t * p = piece_new();
    if(!p) return;
    p->obj = lv_obj_create(root);
    lv_obj_remove_style_all(p->obj);
    piece_place(p, size, col, row);
    lv_obj_set_style_bg_color(p->obj, lv_color_hex(set_colour()), 0);
    lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(p->obj, LV_OPA_TRANSP, 0);
    fade(p->obj, 0, 255, FADE_IN_MS);
}

/* ------------------------------------------------------------ the deck */

static void deck_build(void)
{
    free(deck); deck = NULL; deck_n = deck_pos = 0;
    int cap = 0;
    for(int s = 0; s < libs_count(); s++) cap += libs_get(s)->lib.nalbums;
    if(cap == 0) return;
    deck = calloc((size_t)cap, sizeof(*deck));
    if(!deck) return;
    for(int s = 0; s < libs_count(); s++) {
        const library_t * lib = &libs_get(s)->lib;
        for(int a = 0; a < lib->nalbums; a++)
            if(lib->albums[a].has_art && lib->albums[a].thumb_sizes) { deck[deck_n].src = s; deck[deck_n].album = a; deck_n++; }
    }
    for(int i = deck_n - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); deck_card_t t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
}

static int thumb_size_index(const lib_album_t * al, int want_px)
{
    int si = -1;
    for(int i = 0; i < LIB_THUMB_COUNT; i++) if((al->thumb_sizes & (1 << i)) && lib_thumb_px[i] >= want_px) { si = i; break; }
    if(si < 0) for(int i = LIB_THUMB_COUNT - 1; i >= 0; i--) if(al->thumb_sizes & (1 << i)) { si = i; break; }
    return si;
}

static int next_req_id;

/* The worker's list is every piece still waiting (thumbs_want replaces the
 * whole list, so one request at a time would drop the others). */
static void want_refresh(void)
{
    thumb_req_t reqs[PIECES_MAX];
    int n = 0;
    for(int i = 0; i < npieces; i++) if(pieces[i]->thumb_id >= 0) reqs[n++] = pieces[i]->req;
    thumbs_want(reqs, n);
}

/* Ask the worker for an album's thumbnail; the piece takes it when it lands. */
static piece_t * spawn_cover_piece(int src, int album, int side, int x, int y)
{
    const lib_source_t * s = libs_get(src);
    if(!s) return NULL;
    const lib_album_t * al = &s->lib.albums[album];
    int si = thumb_size_index(al, side);
    if(si < 0) return NULL;
    thumb_req_t req;
    req.id = ++next_req_id;
    req.gen = req_gen;
    req.px = side;
    cache_thumb_path(s->card, al, si, req.path, sizeof(req.path));
    piece_t * p = piece_new();
    if(!p) return NULL;
    p->thumb_id = req.id;
    p->req = req;
    p->obj = lv_image_create(root);
    lv_obj_set_size(p->obj, side, side);
    lv_obj_set_pos(p->obj, x, y);
    lv_image_set_inner_align(p->obj, LV_IMAGE_ALIGN_CENTER);     /* a non-square cover sits on its own ground, the cell stays filled */
    lv_obj_set_style_opa(p->obj, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(p->obj, LV_OBJ_FLAG_HIDDEN);                 /* nothing to draw until the decode lands */
    want_refresh();
    return p;
}

static void spawn_art(void)
{
    if(deck_n == 0) { spawn_square(); return; }
    if(deck_pos >= deck_n) deck_pos = 0;
    deck_card_t c = deck[deck_pos++];
    int size = pick_size(), col, row;
    if(!place(&size, &col, &row)) return;
    int x = cell_x(col), side = cell_x(col + size) - x;
    piece_t * p = spawn_cover_piece(c.src, c.album, side, x, cell_y(row));
    if(!p) { spawn_square(); return; }
    piece_place(p, size, col, row);
    lv_obj_set_style_bg_color(p->obj, lv_color_hex(set_colour()), 0);
    lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, 0);
}

/* A decode landed: give it to the piece that asked. */
static void take_thumbs(void)
{
    thumb_res_t r;
    while(thumbs_poll_reserved(&r)) {
        if(r.gen != req_gen) { free(r.pixels); continue; }
        piece_t * p = NULL; int pi = -1;
        for(int i = 0; i < npieces; i++) if(pieces[i]->thumb_id == r.id) { p = pieces[i]; pi = i; break; }
        if(!p || !r.pixels) { free(r.pixels); if(p) piece_drop(pi); continue; }
        p->thumb_id = -1;
        p->pixels = r.pixels;
        if(p->size <= 0) {
            /* the cover saver: the decode is fitted inside the square - keep its shape, centred in the spot */
            int side = lv_obj_get_width(p->obj), big = r.w > r.h ? r.w : r.h;
            int w = big ? side * r.w / big : side, h = big ? side * r.h / big : side;
            lv_obj_set_pos(p->obj, lv_obj_get_x(p->obj) + (side - w) / 2, lv_obj_get_y(p->obj) + (side - h) / 2);
            lv_obj_set_size(p->obj, w, h);
        }
        p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        p->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        p->dsc.header.w = (uint32_t)r.w;
        p->dsc.header.h = (uint32_t)r.h;
        p->dsc.header.stride = (uint32_t)r.w * 4;
        p->dsc.data_size = (uint32_t)r.w * (uint32_t)r.h * 4;
        p->dsc.data = r.pixels;
        lv_image_set_src(p->obj, &p->dsc);
        lv_obj_remove_flag(p->obj, LV_OBJ_FLAG_HIDDEN);
        p->born_ms = lv_tick_get();
        fade(p->obj, 0, 255, FADE_IN_MS);
        if(p == ball) ball_landed(r.pixels, r.w, r.h);
    }
}

/* ------------------------------------------------------------ cover */

static void pan_x_exec(void * obj, int32_t v) { lv_obj_set_x(obj, v); }
static void pan_y_exec(void * obj, int32_t v) { lv_obj_set_y(obj, v); }

static void cover_show(const lib_source_t * s, int album)
{
    pieces_clear();
    const lib_album_t * al = &s->lib.albums[album];
    uint32_t g = al->palette.ndom ? al->palette.dom[0] : (al->colour ? al->colour : 0x1A1612);
    ground_set(darken(g, 45), ground_now == 0);
    int side = ui_screen.h * 72 / 100;
    int x = (ui_screen.w - side) / 2, y = (ui_screen.h - side) / 2 - ui_screen.h * 4 / 100;
    piece_t * p = al->has_art && al->thumb_sizes ? spawn_cover_piece(cover_src, album, side, x, y) : NULL;
    if(p) {
        lv_obj_set_style_bg_opa(p->obj, LV_OPA_TRANSP, 0);
        /* a slow drift: a few percent across, there and back */
        lv_anim_t a;
        lv_anim_init(&a); lv_anim_set_var(&a, p->obj); lv_anim_set_exec_cb(&a, pan_x_exec);
        lv_anim_set_values(&a, x - ui_screen.w * 3 / 100, x + ui_screen.w * 3 / 100);
        lv_anim_set_duration(&a, COVER_PAN_MS); lv_anim_set_playback_duration(&a, COVER_PAN_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE); lv_anim_start(&a);
        lv_anim_init(&a); lv_anim_set_var(&a, p->obj); lv_anim_set_exec_cb(&a, pan_y_exec);
        lv_anim_set_values(&a, y + ui_screen.h * 2 / 100, y - ui_screen.h * 2 / 100);
        lv_anim_set_duration(&a, COVER_PAN_MS * 13 / 10); lv_anim_set_playback_duration(&a, COVER_PAN_MS * 13 / 10);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE); lv_anim_start(&a);
    }
    uint32_t ink = palette_contrast(ground_to, 0xFAEFD0) >= palette_contrast(ground_to, 0x29221E) ? 0xFAEFD0 : 0x29221E;
    lv_label_set_text(title_lbl, al->title ? al->title : "");
    lv_label_set_text(artist_lbl, al->artist ? al->artist : "");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(artist_lbl, lv_color_hex(mix(ground_to, ink, 70)), 0);
    lv_obj_remove_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(artist_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------ bounce */

static lv_timer_t * ball_timer;
static float    bx, by, bvx, bvy;           /* the cover's corner and speed, px and px/s */
static int      bounces, corner_every = CORNER_EVERY;
static bool     ball_entering;              /* coming in from off screen: the edges don't count yet */
static bool     booming;
static uint32_t boom_started_ms, last_frame_ms, reenter_at_ms, boom_ended_ms, after_until_ms;
static char     last_title[1024], last_artist[1024];
static const lib_source_t * last_s; static int last_album = -1;
static int      boom_cx, boom_cy;           /* the corner it happened in */
static lv_obj_t * boom_img;                 /* the rings, drawn by hand each frame (LVGL's circle masks
                                             * stop at a few hundred px of radius) */
static uint8_t * boom_buf;
static lv_image_dsc_t boom_dsc;
static uint32_t b_ground, b_contrast;       /* the album's colour and the one that fights it most */

static float ball_speed(void) { return (float)ui_screen.w * BALL_SPEED_PCT / 100.0f; }

/* Point the cover at a corner with its speed kept (the next edge it meets
 * is both edges at once). */
static void ball_aim(int pw, int ph)
{
    float tx = bvx > 0 ? (float)(ui_screen.w - pw) : 0.0f;
    float ty = bvy > 0 ? (float)(ui_screen.h - ph) : 0.0f;
    float dx = tx - bx, dy = ty - by, len = sqrtf(dx * dx + dy * dy);
    if(len < 1.0f) return;
    float v = ball_speed();
    bvx = dx / len * v; bvy = dy / len * v;
}

static void ball_launch(float angle_deg, int sx, int sy)
{
    float v = ball_speed(), a = angle_deg * 3.14159265f / 180.0f;
    bvx = cosf(a) * v * (float)sx; bvy = sinf(a) * v * (float)sy;
}

static tile_style_t ball_style;             /* the no-art tile's dealt colourway and face */

static void ball_make(const lib_source_t * s, int album, const char * title, const char * artist)
{
    bool had = ball != NULL;
    bool hidden = (ball && ball->obj && lv_obj_has_flag(ball->obj, LV_OBJ_FLAG_HIDDEN) && booming) || reenter_at_ms;
    if(ball) { for(int i = 0; i < npieces; i++) if(pieces[i] == ball) { piece_drop(i); break; } }
    ball = NULL;
    int side = ui_screen.h * BALL_PCT / 100;
    if(!had && !reenter_at_ms && !booming) {
        /* the first cover: somewhere on the screen, off at an angle (a
         * change of track keeps the place, the speed and the count) */
        bx = (float)rnd_between(0, ui_screen.w - side); by = (float)rnd_between(0, ui_screen.h - side);
        ball_launch((float)rnd_between(25, 65), rnd() % 2 ? 1 : -1, rnd() % 2 ? 1 : -1);
        ball_entering = false;
    }
    const lib_album_t * al = s && album >= 0 ? &s->lib.albums[album] : NULL;
    int src = -1;
    for(int i = 0; i < libs_count() && s; i++) if(libs_get(i) == s) src = i;
    if(al && al->has_art && al->thumb_sizes && src >= 0) ball = spawn_cover_piece(src, album, side, (int)bx, (int)by);
    if(!ball) {
        ball = piece_new();
        if(!ball) return;
        ball->obj = lv_obj_create(root);
        lv_obj_remove_style_all(ball->obj);
        lv_obj_set_size(ball->obj, side, side);
        lv_obj_set_pos(ball->obj, (int)bx, (int)by);
        lv_obj_set_style_bg_opa(ball->obj, LV_OPA_COVER, 0);
        lv_obj_remove_flag(ball->obj, LV_OBJ_FLAG_SCROLLABLE);
        /* no art: the grid's no-art tile, dealt for this track (Ian, 17:23:
         * the same colours, faces, wrapping and sizing as the album
         * squares; the track's title in the album's place; the artist up to
         * two lines, the title three; plus the sign's inset keyline) */
        lv_obj_set_style_bg_color(ball->obj, lv_color_hex(ball_style.bg), 0);
        lv_obj_t * k = ui_sign(ball->obj);
        ui_sign_colour(k, ball_style.ink);
        tiletext_t tt;
        tiletext_make_lines(ball->obj, side, title ? title : "", artist ? artist : "", &ball_style, 3, &tt);
        lv_obj_set_style_opa(ball->obj, LV_OPA_TRANSP, 0);
        fade(ball->obj, 0, 255, FADE_IN_MS);
    }
    if(hidden) lv_obj_add_flag(ball->obj, LV_OBJ_FLAG_HIDDEN);
}

/* The partner of the ground: of the candidates, the most colourful one
 * that reads against it at the theme's own floor (tilecontrast - Ian,
 * 17:50: a mix of the palettes' colours, not cream and dark every time);
 * none at the floor → the one that reads best; nothing → cream or dark. */
static uint32_t pick_contrast(uint32_t ground, const uint32_t * cand, int n, uint32_t never)
{
    double floor = tilestyle_min_contrast();
    uint32_t best = 0, fallback = 0; double bs = -1, bc = -1;
    for(int i = 0; i < n; i++) {
        if(!cand[i] || cand[i] == never || cand[i] == ground) continue;
        double c = palette_contrast(ground, cand[i]);
        if(c > bc) { bc = c; fallback = cand[i]; }
        if(c < floor) continue;
        double sat = tilestyle_saturation(cand[i]) + c * 0.02;      /* colour first, a little weight to readability */
        if(sat > bs) { bs = sat; best = cand[i]; }
    }
    if(best) return best;
    if(fallback && bc >= floor * 0.8) return fallback;
    return palette_contrast(ground, 0xFAEFD0) >= palette_contrast(ground, 0x29221E) ? 0xFAEFD0 : 0x29221E;
}


/* The album's colour as the ground and, from its palette, its partner. */
static void ball_palette_colours(const art_palette_t * pal, uint32_t fallback)
{
    uint32_t g = pal->ndom ? pal->dom[0] : (fallback ? fallback : set_colour());
    b_ground = darken(g, 45);
    uint32_t cand[PAL_DOM + 4]; int n = 0;
    for(int i = 1; i < pal->ndom; i++) cand[n++] = pal->dom[i];
    cand[n++] = pal->bright; cand[n++] = pal->dark; cand[n++] = pal->accent; cand[n++] = pal->accent2;
    b_contrast = pick_contrast(b_ground, cand, n, 0);
}

/* The cover's decode landed: an index without colours gets them read off the picture. */
static void ball_landed(const uint8_t * xrgb, int w, int h)
{
    if(!ball_wants_palette) return;
    ball_wants_palette = false;
    art_palette_t pal;
    if(!palette_extract(xrgb, w, h, &pal)) return;
    ball_palette_colours(&pal, 0);
    if(!booming && !reenter_at_ms) ground_set(b_ground, true);
}

static void ball_colours(const lib_source_t * s, int album)
{
    ball_wants_palette = false;
    const lib_album_t * al = s && album >= 0 ? &s->lib.albums[album] : NULL;
    bool art = al && al->has_art && al->thumb_sizes;
    if(!art) {
        /* a tile: the ground is another colour of its set, darkened; the
         * partner another of the set's colours, or its ink (never the
         * tile's own colour, so the tile always stands out) */
        tile_style_t comp; tilestyle_companion(1, &ball_style, &comp);
        b_ground = darken(comp.bg != ball_style.bg ? comp.bg : ball_style.ink, 35);
        uint32_t cand[20]; int n = 0;
        for(int set = 0; set < tilestyle_set_count() && n == 0; set++) {
            uint32_t cols[16]; int c = tilestyle_set(set, cols, 16);
            bool mine = false;
            for(int i = 0; i < c; i++) if(cols[i] == ball_style.bg) mine = true;
            if(mine) for(int i = 0; i < c && n < 16; i++) cand[n++] = cols[i];
        }
        cand[n++] = ball_style.ink;
        b_contrast = pick_contrast(b_ground, cand, n, ball_style.bg);
        return;
    }
    ball_palette_colours(&al->palette, al->colour);
    ball_wants_palette = al->palette.ndom == 0;          /* an index without colours: read them off the decode when it lands */
}

static void bounce_show(const lib_source_t * s, int album, const char * title, const char * artist)
{
    snprintf(last_title, sizeof(last_title), "%s", title ? title : "");
    snprintf(last_artist, sizeof(last_artist), "%s", artist ? artist : "");
    last_s = s; last_album = album;
    /* the tile's colourway and face, steady per track (the deal's seed is the title) */
    unsigned seed = 2166136261u;
    for(const char * p = last_title; *p; p++) seed = (seed ^ (unsigned char)*p) * 16777619u;
    bool noart = true; int letters = (int)strlen(last_title);
    ball_style.bg = 0x8A7F70; ball_style.ink = 0xFAEFD0; ball_style.font = TILE_FONT_NONE;
    tilestyle_deal(seed, 1, 1, &noart, &letters, &ball_style);
    ball_colours(s, album);
    if(!booming) ground_set(b_ground, true);
    ball_make(s, album, title, artist);
}

static void boom_begin(void)
{
    booming = true;
    boom_started_ms = lv_tick_get();
    boom_cx = bx <= 1.0f ? 0 : ui_screen.w;
    boom_cy = by <= 1.0f ? 0 : ui_screen.h;
    if(ball && ball->obj) lv_obj_add_flag(ball->obj, LV_OBJ_FLAG_HIDDEN);
    free(boom_buf);
    boom_buf = malloc((size_t)ui_screen.w * (size_t)ui_screen.h * 4);
    if(boom_buf) {
        boom_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        boom_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        boom_dsc.header.w = (uint32_t)ui_screen.w;
        boom_dsc.header.h = (uint32_t)ui_screen.h;
        boom_dsc.header.stride = (uint32_t)ui_screen.w * 4;
        boom_dsc.data_size = (uint32_t)ui_screen.w * (uint32_t)ui_screen.h * 4;
        boom_dsc.data = boom_buf;
        lv_image_set_src(boom_img, &boom_dsc);
        lv_obj_remove_flag(boom_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(boom_img);
    }
    fprintf(stderr, "simpleton-ui: saver: bounce - the corner (after %d bounces; ground %06X, partner %06X)\n", bounces, b_ground, b_contrast);
}

static void boom_end(void)
{
    lv_obj_add_flag(boom_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(boom_img, NULL);
    free(boom_buf); boom_buf = NULL;
}

/* One frame of the rings: every pixel's band is its distance from the
 * corner; a row is a few square roots and some span fills. */
static void boom_draw(uint32_t t)
{
    if(!boom_buf) return;
    int W = ui_screen.w, H = ui_screen.h;
    float gap = (float)H / (float)BOOM_RINGS;
    float reach = sqrtf((float)W * W + (float)H * H);
    /* rings keep coming out of the corner for the whole time, each one
     * crossing the screen in BOOM_SWEEP_MS; beyond the first ring's front
     * the ground flashes */
    float front = reach * (float)t / (float)BOOM_SWEEP_MS;
    int nb = (int)(front / gap) + 1;                  /* boundaries from the front inwards */
    if(nb > 64) nb = 64;
    uint32_t flash = (t / BOOM_FLASH_MS) % 2 ? b_ground : b_contrast;
    uint32_t band[2] = { b_contrast, b_ground };
    uint32_t * px = (uint32_t *)boom_buf;
    for(int y = 0; y < H; y++) {
        float dy = (float)(y - boom_cy);
        float dy2 = dy * dy;
        int xb[65];
        for(int j = 0; j <= nb; j++) {
            float R = front - gap * (float)j;
            xb[j] = R > 0 && R * R > dy2 ? (int)sqrtf(R * R - dy2) : 0;
            if(xb[j] > W) xb[j] = W;
        }
        uint32_t * row = px + (size_t)y * (size_t)W;
        int lo = 0;
        for(int j = nb; j >= 0; j--) {                 /* from the corner outwards: band j lies inside boundary j */
            int hi = xb[j];
            uint32_t c = band[j % 2];
            for(int x = lo; x < hi; x++) row[boom_cx == 0 ? x : W - 1 - x] = c;
            lo = hi;
        }
        for(int x = lo; x < W; x++) row[boom_cx == 0 ? x : W - 1 - x] = flash;
    }
    lv_obj_invalidate(boom_img);
}

static void boom_tick(uint32_t now)
{
    uint32_t t = now - boom_started_ms;
    if(t >= BOOM_MS) {
        /* over: the colours turn round; a pause, then the cover comes back from the opposite corner */
        booming = false;
        boom_end();
        uint32_t tmp = b_ground; b_ground = b_contrast; b_contrast = tmp;
        boom_ended_ms = now;
        after_until_ms = now + BOOM_AFTER_MS;                  /* a few whole-screen flashes first */
        reenter_at_ms = now + BOOM_REST_MS;
        if(reenter_at_ms == 0) reenter_at_ms = 1;
        return;
    }
    /* the ground flashes between the two colours; rings of the two, in
     * turn, grow out of the corner and leave the screen */
    boom_draw(t);
}

static void ball_tick(lv_timer_t * t)
{
    (void)t;
    if(kind != SAVER_BOUNCE) return;
    uint32_t now = lv_tick_get();
    float dt = (float)(now - last_frame_ms) / 1000.0f;
    last_frame_ms = now;
    if(dt > 0.1f) dt = 0.1f;
    if(booming) { boom_tick(now); return; }
    if(reenter_at_ms) {
        if(after_until_ms) {
            if((int32_t)(now - after_until_ms) < 0) {
                lv_obj_set_style_bg_color(stage, lv_color_hex(((now - boom_ended_ms) / 200) % 2 ? b_contrast : b_ground), 0);
                return;
            }
            after_until_ms = 0;
            ground_set(b_ground, true);                        /* the new ground settles; the cover is on its way */
        }
        if((int32_t)(now - reenter_at_ms) < 0) return;
        reenter_at_ms = 0;
        if(ball && ball->obj) {
            int pw = lv_obj_get_width(ball->obj), ph = lv_obj_get_height(ball->obj);
            bx = boom_cx == 0 ? (float)ui_screen.w : (float)-pw;
            by = boom_cy == 0 ? (float)ui_screen.h : (float)-ph;
            ball_launch((float)rnd_between(30, 60), boom_cx == 0 ? -1 : 1, boom_cy == 0 ? -1 : 1);
            lv_obj_set_pos(ball->obj, (int)bx, (int)by);
            lv_obj_remove_flag(ball->obj, LV_OBJ_FLAG_HIDDEN);
        }
        ball_entering = true;
        bounces = 0;
    }
    if(!ball && cover_file[0]) ball_make(last_s, last_album, last_title, last_artist);   /* a decode that failed: the tile instead */
    if(!ball || !ball->obj || lv_obj_has_flag(ball->obj, LV_OBJ_FLAG_HIDDEN)) return;
    int pw = lv_obj_get_width(ball->obj), ph = lv_obj_get_height(ball->obj);
    int W = ui_screen.w, H = ui_screen.h;
    bx += bvx * dt; by += bvy * dt;
    if(ball_entering) {
        if(bx >= 0 && by >= 0 && bx + pw <= W && by + ph <= H) ball_entering = false;
        lv_obj_set_pos(ball->obj, (int)bx, (int)by);
        return;
    }
    bool hx = false, hy = false;
    if(bx <= 0) { bx = 0; bvx = fabsf(bvx); hx = true; }
    else if(bx + pw >= W) { bx = (float)(W - pw); bvx = -fabsf(bvx); hx = true; }
    if(by <= 0) { by = 0; bvy = fabsf(bvy); hy = true; }
    else if(by + ph >= H) { by = (float)(H - ph); bvy = -fabsf(bvy); hy = true; }
    if(hx || hy) {
        bool at_x = bx <= 2.0f || bx + pw >= W - 2, at_y = by <= 2.0f || by + ph >= H - 2;
        if(at_x && at_y) { lv_obj_set_pos(ball->obj, (int)bx, (int)by); boom_begin(); return; }
        bounces++;
        if(bounces == corner_every - 1) ball_aim(pw, ph);       /* the next edge is a corner */
    }
    lv_obj_set_pos(ball->obj, (int)bx, (int)by);
}

/* ------------------------------------------------------------ the track */

/* The playing track, when it changed: its album (or none). */
/* The album a file belongs to: by its folder's entry, else by the albums' folders. */
static void lookup_album(const char * file, const lib_source_t ** out_s, int * out_album)
{
    char folder[1024];
    snprintf(folder, sizeof(folder), "%s", file);
    char * slash = strrchr(folder, '/');
    if(slash) *slash = 0; else folder[0] = 0;
    const lib_folder_t * fo = NULL;
    const lib_source_t * s = libs_find_folder(folder, &fo);
    int src = -1, album = fo ? fo->album : -1;
    for(int i = 0; i < libs_count() && s; i++) if(libs_get(i) == s) src = i;
    if(album < 0) {
        for(int i = 0; i < libs_count() && album < 0; i++) {
            const lib_source_t * c = libs_get(i);
            for(int k = 0; k < c->lib.nalbums; k++)
                if(c->lib.albums[k].folder && strcmp(c->lib.albums[k].folder, folder) == 0) { s = c; src = i; album = k; break; }
        }
    }
    if(!s || album < 0 || src < 0) { *out_s = NULL; *out_album = -1; return; }
    *out_s = s; *out_album = album;
}

static bool poll_track(const lib_source_t ** out_s, int * out_album, mpd_status_t * st)
{
    if(!mpd_status(st) || st->songid < 0 || !st->file[0]) return false;
    if(strcmp(st->file, cover_file) == 0) return false;
    snprintf(cover_file, sizeof(cover_file), "%s", st->file);
    lookup_album(st->file, out_s, out_album);
    return true;
}

/* The playing album's colours for a classic visualizer's "record" look
 * (classic.c, 0.32): the album's colour set back as the ground, its partner
 * (Bounce's rule) for the bars, its brightest for the caps. False when
 * nothing plays or the album has no colours. */
bool saver_playing_colours(uint32_t * ground, uint32_t * bar, uint32_t * cap, char * file, size_t file_len)
{
    mpd_status_t st;
    if(!mpd_status(&st) || st.songid < 0 || !st.file[0]) return false;
    snprintf(file, file_len, "%s", st.file);
    const lib_source_t * s = NULL; int album = -1;
    lookup_album(st.file, &s, &album);
    const lib_album_t * al = s && album >= 0 ? &s->lib.albums[album] : NULL;
    if(!al || al->palette.ndom == 0) return false;
    const art_palette_t * pal = &al->palette;
    *ground = darken(pal->dom[0], 45);
    uint32_t cand[PAL_DOM + 4]; int n = 0;
    for(int i = 1; i < pal->ndom; i++) cand[n++] = pal->dom[i];
    cand[n++] = pal->bright; cand[n++] = pal->dark; cand[n++] = pal->accent; cand[n++] = pal->accent2;
    *bar = pick_contrast(*ground, cand, n, 0);
    *cap = pal->bright && palette_contrast(*ground, pal->bright) >= 3.0 ? pal->bright : 0xFAEFD0;
    return true;
}

static void track_poll(void)
{
    mpd_status_t st; const lib_source_t * s = NULL; int album = -1;
    if(!poll_track(&s, &album, &st)) return;
    const char * slash = strrchr(st.file, '/');
    const char * title = st.title[0] ? st.title : (slash ? slash + 1 : st.file);
    if(kind == SAVER_BOUNCE) { bounce_show(s, album, title, st.artist); return; }
    if(!s || album < 0) {
        /* no album known: the ground alone, the file's name */
        fprintf(stderr, "simpleton-ui: saver: no album for %s\n", st.file);
        pieces_clear();
        lv_label_set_text(title_lbl, title);
        lv_label_set_text(artist_lbl, st.artist);
        return;
    }
    cover_src = -1; cover_album = album;
    for(int i = 0; i < libs_count(); i++) if(libs_get(i) == s) cover_src = i;
    cover_show(s, album);
}

/* ------------------------------------------------------------ the tick */

static void tick(lv_timer_t * t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    take_thumbs();
    ground_tick(now);
    bool landed = false;
    for(int i = 0; i < npieces; ) {
        piece_t * p = pieces[i];
        if(p->thumb_id >= 0 && now - p->born_ms >= 15000) { piece_drop(i); want_refresh(); continue; }   /* a decode that never came */
        if(!p->solid && p->thumb_id < 0 && now - p->born_ms >= FADE_IN_MS + 100) { p->solid = true; landed = true; }
        i++;
    }
    if(landed) drop_hidden();
    if(kind == SAVER_DRIFT || kind == SAVER_PALETTES) {
        if(now - last_spawn_ms >= SPAWN_MS) {
            last_spawn_ms = now;
            if(kind == SAVER_PALETTES || rnd() % 5 < 2) spawn_square(); else spawn_art();
        }
        if((int32_t)(now - next_set_ms) >= 0) {
            pick_set();                                        /* the scheme moves on; the squares carry it */
            next_set_ms = now + (uint32_t)rnd_between(SET_MIN_MS, SET_MAX_MS);
        }
    }
    else if((kind == SAVER_COVER || kind == SAVER_BOUNCE) && now - last_poll_ms >= COVER_POLL_MS) { last_poll_ms = now; track_poll(); }
}

/* ------------------------------------------------------------ public */

void saver_create(void)
{
    root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    stage = lv_obj_create(root);
    lv_obj_remove_style_all(stage);
    lv_obj_set_size(stage, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_COVER, 0);
    title_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(title_lbl, ui_font_caps(26), 0);
    lv_obj_set_width(title_lbl, ui_screen.w * 60 / 100);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lbl, LV_ALIGN_BOTTOM_LEFT, ui_screen.w * 5 / 100, -ui_screen.h * 8 / 100);
    artist_lbl = lv_label_create(root);
    lv_obj_set_style_text_font(artist_lbl, ui_font(22), 0);
    lv_obj_set_width(artist_lbl, ui_screen.w * 60 / 100);
    lv_label_set_long_mode(artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(artist_lbl, LV_ALIGN_BOTTOM_LEFT, ui_screen.w * 5 / 100, -ui_screen.h * 4 / 100);
    boom_img = lv_image_create(root);
    lv_obj_set_size(boom_img, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(boom_img, 0, 0);
    lv_obj_add_flag(boom_img, LV_OBJ_FLAG_HIDDEN);
    classic_create(root);
    ball_timer = lv_timer_create(ball_tick, BALL_FRAME_MS, NULL);
    lv_timer_pause(ball_timer);
    rnd_state = (unsigned)lv_tick_get() ^ 0x5bd1e995u;
}

static bool starting_manual;                                 /* saver_start_manual is the caller */

/* the playing track is DST (dst.h) */
static bool track_is_dst(void)
{
    mpd_status_t st;
    if(!mpd_status(&st) || !st.file[0]) return false;
    return dst_track(st.file);
}

void saver_start(saver_kind_t k)
{
    if(!root || k == SAVER_NONE) return;
    if(kind != SAVER_NONE) saver_stop();
    kind = k;
    manual = false;
    req_gen++;
    thumbs_reserve(true);
    pieces_clear();
    cover_file[0] = 0;
    lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(artist_lbl, LV_OBJ_FLAG_HIDDEN);
    uint32_t now = lv_tick_get();
    ground_now = 0;
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_size(stage, ui_screen.w, ui_screen.h);
    if(saver_kind_is_viz(k)) {
        /* not on a DST track (Ian, 19 Oct 00:13: decoding DST already runs
         * the chip near its limit; the lighter savers instead) */
        if(track_is_dst()) {
            fprintf(stderr, "simpleton-ui: saver: %s held back, a DST track plays%s\n", saver_kind_code(k), starting_manual ? "" : " - art drift instead");
            kind = SAVER_NONE; thumbs_reserve(false);
            if(starting_manual) objmenu_toast(T(S_VIZ_NO_DST));
            else saver_start(SAVER_DRIFT);
            return;
        }
        if(k != SAVER_STAGE) {
            /* a classic (0.32 / 0.33): drawn by LVGL inside our root, in its chosen look */
            char key[48], look[32];
            snprintf(key, sizeof(key), "viz-%s-look", saver_kind_code(k));
            config_read(key, look, sizeof(look), "archer");
            if(!classic_start(classic_kind_from_code(saver_kind_code(k)), look)) {
                fprintf(stderr, "simpleton-ui: saver: %s unavailable, art drift instead\n", saver_kind_code(k));
                kind = SAVER_NONE; thumbs_reserve(false);
                saver_start(SAVER_DRIFT);
                return;
            }
            lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(root);
            if(!timer) timer = lv_timer_create(tick, TICK_MS, NULL);
            lv_timer_pause(timer);
            fprintf(stderr, "simpleton-ui: saver: %s\n", saver_kind_code(k));
            return;
        }
        /* a visualizer (0.31): its own plane above the UI; the LVGL root stays hidden */
        if(!viz_start(VIZ_SOUNDSTAGE)) {
            fprintf(stderr, "simpleton-ui: saver: soundstage unavailable, art drift instead\n");
            kind = SAVER_NONE; thumbs_reserve(false);
            saver_start(SAVER_DRIFT);
            return;
        }
        if(!timer) timer = lv_timer_create(tick, TICK_MS, NULL);
        lv_timer_pause(timer);
        /* the UI is out of sight: nothing is drawn until the visualizer ends
         * (0.31i - a hidden now-playing redraw at every skip was the busiest
         * moment on the unit, and skips were where DST tracks blipped) */
        lv_display_enable_invalidation(NULL, false);
        fprintf(stderr, "simpleton-ui: saver: stage\n");
        return;
    }
    if(k == SAVER_OFF) ground_set(0x000000, true);
    else if(k == SAVER_COVER) { ground_set(0x1A1612, true); last_poll_ms = 0; }
    else if(k == SAVER_BOUNCE) {
        { char n[16]; config_read("viz-bounce-n", n, sizeof(n), "1000"); corner_every = atoi(n) > 1 ? atoi(n) : CORNER_EVERY;
          const char * e = getenv("SIMPLETON_BOUNCE_EVERY"); if(e && atoi(e) > 1) corner_every = atoi(e); }
        libs_refresh();
        pick_set();
        ground_set(0x1A1612, true); last_poll_ms = 0;
        ball = NULL; booming = false; bounces = 0; last_frame_ms = now; reenter_at_ms = 0; last_s = NULL; last_album = -1;
        lv_timer_resume(ball_timer);
    }
    else {
        libs_refresh();
        deck_build();
        lv_obj_set_pos(stage, 0, bar_h());                   /* the 2:1 stage between the black bars */
        lv_obj_set_size(stage, ui_screen.w, stage_h());
        ground_set(pick_ground(), true);
        next_set_ms = now + (uint32_t)rnd_between(SET_MIN_MS, SET_MAX_MS);
        last_spawn_ms = now - SPAWN_MS;                      /* the first piece at once */
    }
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    if(!timer) timer = lv_timer_create(tick, TICK_MS, NULL);
    lv_timer_resume(timer);
    if(k == SAVER_DRIFT) fprintf(stderr, "simpleton-ui: saver: drift (%d covers in the cache)\n", deck_n);
    else fprintf(stderr, "simpleton-ui: saver: %s\n", saver_kind_code(k));
}

void saver_set_bounce_every(int n) { if(n > 1) corner_every = n; }

void saver_stop(void)
{
    if(kind == SAVER_NONE) return;
    vizmenu_close();
    fprintf(stderr, "simpleton-ui: saver: ended\n");
    kind = SAVER_NONE;
    manual = false;
    if(classic_active()) classic_stop();
    else if(viz_active()) {
        viz_stop();
        if(!power_screen_off()) {                            /* the UI draws again, whole */
            lv_display_enable_invalidation(NULL, true);
            lv_obj_invalidate(lv_screen_active());
            lv_obj_invalidate(lv_layer_top());
        }
        nowplaying_mpd_changed();                            /* the cover it held back while out of sight */
    }
    if(timer) lv_timer_pause(timer);
    if(ball_timer) lv_timer_pause(ball_timer);
    if(booming) { booming = false; boom_end(); }
    reenter_at_ms = 0;
    pieces_clear();
    thumbs_want(NULL, 0);
    thumbs_reserve(false);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
}

void saver_start_manual(saver_kind_t k)
{
    starting_manual = true;
    saver_start(k);
    starting_manual = false;
    manual = kind != SAVER_NONE;
}

/* The player changed (main.c): a visualizer running into a DST track ends
 * - back to the page it was started from, or on to Art drift for the TV's
 * with-music rule (0.31j). */
void saver_mpd_changed(void)
{
    if(!saver_kind_is_viz(kind) || !track_is_dst()) return;
    bool was_manual = manual;
    fprintf(stderr, "simpleton-ui: saver: a DST track started, soundstage ends%s\n", was_manual ? "" : " - art drift instead");
    saver_stop();
    if(was_manual) objmenu_toast(T(S_VIZ_NO_DST));
    else saver_start(SAVER_DRIFT);
}

bool saver_manual(void) { return manual; }
bool saver_kind_is_viz(saver_kind_t k) { return k == SAVER_STAGE || k == SAVER_SPECTRUM || k == SAVER_METERS || k == SAVER_SCOPE || k == SAVER_SPECTROGRAM; }
bool saver_active(void) { return kind != SAVER_NONE; }
saver_kind_t saver_current(void) { return kind; }

saver_kind_t saver_kind_from_code(const char * code)
{
    if(!code) return SAVER_OFF;
    if(strcmp(code, "drift") == 0) return SAVER_DRIFT;
    if(strcmp(code, "cover") == 0) return SAVER_COVER;
    if(strcmp(code, "palettes") == 0) return SAVER_PALETTES;
    if(strcmp(code, "bounce") == 0) return SAVER_BOUNCE;
    if(strcmp(code, "stage") == 0) return SAVER_STAGE;
    if(strcmp(code, "spectrum") == 0) return SAVER_SPECTRUM;
    if(strcmp(code, "meters") == 0) return SAVER_METERS;
    if(strcmp(code, "scope") == 0) return SAVER_SCOPE;
    if(strcmp(code, "spectrogram") == 0) return SAVER_SPECTROGRAM;
    return SAVER_OFF;
}
const char * saver_kind_code(saver_kind_t k)
{
    switch(k) { case SAVER_DRIFT: return "drift"; case SAVER_COVER: return "cover"; case SAVER_PALETTES: return "palettes"; case SAVER_BOUNCE: return "bounce"; case SAVER_STAGE: return "stage"; case SAVER_SPECTRUM: return "spectrum"; case SAVER_METERS: return "meters"; case SAVER_SCOPE: return "scope"; case SAVER_SPECTROGRAM: return "spectrogram"; case SAVER_OFF: return "off"; default: return "none"; }
}
