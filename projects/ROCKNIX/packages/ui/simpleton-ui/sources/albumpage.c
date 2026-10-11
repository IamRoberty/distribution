/*
 * SimpletonOS UI - the album page (implementation). See albumpage.h and
 * album-page-6b.md.
 *
 * Measurements: the handheld page is drawn in stage units (PX, 1/720 of
 * the stage); the TV page in 1080ths of its stage (tvpx), the size the
 * design was drawn at, so a 720p TV gets the same composition smaller.
 */
#include "albumpage.h"
#include "art.h"
#include "cache.h"
#include "config.h"
#include "mpdc.h"
#include "nowplaying.h"
#include "palette.h"
#include "placeholder.h"
#include "strings.h"
#include "textfit.h"
#include "theme.h"
#include "thumbs.h"
#include "tilestyle.h"
#include "tiletext.h"
#include "src/misc/cache/instance/lv_image_cache.h"   /* lv_image_cache_drop, not in lvgl.h */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define GROW_PCT      108       /* the selected title's growth */
#define TV_BASE       1080      /* the TV design's stage */
#define SCROLL_MS     110
#define MAX_LINES     3         /* a title wraps to this many lines at most */
#define FAV_RED       0xE0242C  /* hard-wired, Simpleton's favourite colour (theme.h) */
#define MARK_BUFS     64
#define FRAME_W       10        /* the selected cover's border on the TV, in 1080ths */

/* ---- the album, copied at open ---- */

typedef struct {
    char *     uri, * title;
    int        disc, track;
    long       rank;             /* the format's quality rank (library_format_rank): the better copy wins */
    bool       fav;
    /* laid out for this visit */
    int        side;             /* 0, or 1 on the right of a split */
    int        y, h, lines;      /* in its side's content */
    int        tx0, tw0;         /* first line: start x in the column and width, at the normal size */
    int        tx1, tw1;         /* ... and at the grown size */
    lv_obj_t * num, * lab, * heart;
    char       wrapped[400];
} track_t;

static struct {
    char   title[300], artist[300], folder[1024], card[64], thumb[320];
    bool   has_art;
    uint32_t colour;             /* the cover's preview colour */
    art_palette_t palette;
    bool   has_tile;             /* the grid showed a no-art tile: its style carries over (Ian, 10 Oct) */
    tile_style_t tile;
    track_t * tr;
    int    n, discs;
} alb;

/* ---- the page ---- */

static lv_obj_t * scr;
static lv_obj_t * page;              /* everything the page draws, under now-playing's stage when that is hosted here */
static lv_obj_t * frame;             /* TV: the border round the cover when it is selected */
static lv_obj_t * art_img, * field[2], * content[2], * mark;
static lv_timer_t * poll_timer;
static void (*exit_cb)(void);
static void (*play_cb)(void);        /* handheld: a track started from here (main shows now-playing) */
static int  sq_x;                    /* TV: the cover square's x */
static char last_song[1024];         /* MPD's current file at the last look: a change pops the controls */

/* TV: who has the D-pad - the list, or the cover (Ian, 10 Oct 19:33). On
 * the cover: Up/Down bring now-playing's controls up (then they have the
 * D-pad until Back or the idle fade), Select turns the cover's pages (the
 * frame goes, the "n / m" badge flashes), Left/Right go back to the list.
 * Whenever the controls are up, Back takes them down. */
typedef enum { FOCUS_LIST, FOCUS_ART } focus_t;
static focus_t focus;
static bool    art_paged;            /* Select has turned a page: the frame stays off until the cover is left and re-entered */

static bool     shown, opened, tv;
static unsigned seed;
static int      gen;                 /* thumbnail request generation */
static uint8_t * art_px;
static lv_image_dsc_t art_dsc;

static int sel, playing;             /* track indices; playing -1 = none */

/* TV, an album without art: what the square shows this visit (Ian, 10 Oct
 * 20:49) - the no-art tile as the grid showed it, or a cassette, or a
 * record, in the tile's colours. The picture is drawn off the UI thread
 * (a stage-sized tint and its lettering: tens of ms on the device), the
 * latest job wins; while it is on its way the square is the tile's colour. */
static const char * square_design;   /* NULL = the tile */
static int          last_square = -1;
static placeholder_tint_t sq_tints[8];
static int          sq_ntints;
static uint32_t     sq_ink;
static char         sq_font[64];     /* the album line's face: the tile's ("" = the theme's) */
static bool         sq_caps;
static pthread_mutex_t sq_lock = PTHREAD_MUTEX_INITIALIZER;
static int          sq_gen;          /* bumped per job and at hide: an older job's picture is dropped */
static struct { int gen; uint8_t * px; int w, h; bool ready; } sq_res;

static const page_layout_t * lay;
static page_layout_t lay_default;
static uint32_t c_bg, c_ink, c_text, c_play, c_sel;   /* ink: names, numbers; text: unselected tracks; sel: the selected one (= the legible ink) */
static tile_style_t tst[2];          /* [0] the no-art square, [1] the page */
static int split;                    /* the first track of the right side; n = no split */

static const char * liner_file;      /* NULL = the UI face */
static bool liner_caps;
static int  liner_leading;
static int  lsize, lh, row_gap, gutter, lead;   /* lead: room left of the gutter for the playing mark */
static const lv_font_t * f_list, * f_grown;
static int  col_x[2], col_w[2], col_y, col_h;   /* each side's title column on the screen */
static int  scroll_y[2], total_h[2];

static lv_draw_buf_t * mark_bufs[MARK_BUFS];
static int nmark_bufs;

static int tvpx(int v) { return (v * ui_screen.stage + TV_BASE / 2) / TV_BASE; }
static int sz(int tv_units, int hh_units) { return tv ? tvpx(tv_units) : PX(hh_units); }

/* ---- the copy ---- */

static void album_free(void)
{
    for(int i = 0; i < alb.n; i++) { free(alb.tr[i].uri); free(alb.tr[i].title); }
    free(alb.tr);
    memset(&alb, 0, sizeof(alb));
}

static int track_cmp(const void * pa, const void * pb)
{
    const track_t * a = pa, * b = pb;
    if(a->disc != b->disc) return a->disc < b->disc ? -1 : 1;
    if(a->track != b->track) return a->track < b->track ? -1 : 1;
    return 0;
}

/* The palette from the smallest thumbnail, when the index has none yet
 * (an index written by a cache tool from before 0.17 - the installed one,
 * until the next image - or a cover scanned since). A 192 px decode: a few
 * ms. */
static void palette_from_thumb(const lib_album_t * a)
{
    char path[320];
    cache_thumb_path(alb.card, a, 0, path, sizeof(path));
    FILE * f = fopen(path, "rb");
    if(!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t * data = (len > 0 && len < (1 << 20)) ? malloc((size_t)len) : NULL;
    bool ok = data && fread(data, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if(!ok) { free(data); return; }
    int w = 0, h = 0;
    uint8_t * px = art_decode_fit(data, (size_t)len, lib_thumb_px[0], &w, &h);
    free(data);
    if(!px) return;
    if(palette_extract(px, w, h, &alb.palette)) fprintf(stderr, "simpleton-ui: album page: palette for \"%s\" from its thumbnail (the index has none)\n", alb.title);
    free(px);
}

bool albumpage_open(const library_t * lib, int album, const char * card, const tile_style_t * tile)
{
    if(!lib || album < 0 || album >= lib->nalbums) return false;
    const lib_album_t * a = &lib->albums[album];
    if(a->track_count <= 0) return false;
    album_free();
    snprintf(alb.title, sizeof(alb.title), "%s", a->title);
    snprintf(alb.artist, sizeof(alb.artist), "%s", a->various ? T(S_VARIOUS_ARTISTS) : a->artist);
    snprintf(alb.folder, sizeof(alb.folder), "%s", a->folder);
    snprintf(alb.card, sizeof(alb.card), "%s", card ? card : "");
    alb.has_art = a->has_art;
    alb.colour = a->colour;
    alb.palette = a->palette;
    alb.has_tile = tile != NULL;
    if(tile) alb.tile = *tile;
    cache_thumb_path(alb.card, a, LIB_THUMB_COUNT - 1, alb.thumb, sizeof(alb.thumb));
    if(alb.has_art && !palette_complete(&alb.palette)) palette_from_thumb(a);
    alb.tr = calloc((size_t)a->track_count, sizeof(track_t));
    if(!alb.tr) { memset(&alb, 0, sizeof(alb)); return false; }
    bool numbered = true;
    for(int i = a->first_track; i < lib->ntracks && alb.n < a->track_count; i++) {
        const lib_track_t * t = &lib->tracks[i];
        if(t->album != album) continue;
        track_t * p = &alb.tr[alb.n++];
        p->uri = strdup(t->uri);
        p->title = strdup(t->title[0] ? t->title : "");
        p->disc = t->disc;
        p->track = t->track;
        p->rank = library_format_rank(t->format);
        if(t->track <= 0) numbered = false;
    }
    if(numbered && alb.n > 1) qsort(alb.tr, (size_t)alb.n, sizeof(track_t), track_cmp);
    /* the same track in two places (a second rip of the album in another
     * folder joins the same album): one of each shows and plays (Ian,
     * 10 Oct 20:09) - the same disc and number with the same title, or the
     * same title where numbers are missing; the first in library order wins */
    int kept = 0;
    for(int i = 0; i < alb.n; i++) {
        int dup = -1;
        for(int k = 0; k < kept && dup < 0; k++) {
            const track_t * a = &alb.tr[k], * b = &alb.tr[i];
            if(a->disc != b->disc || a->track != b->track) continue;
            if(!a->title[0] || !b->title[0]) {
                /* untitled: only a repeated number says "the same"; untitled and
                 * unnumbered tracks are never folded (a plain folder of files) */
                if(a->title[0] != b->title[0] || a->track <= 0) continue;
            }
            else if(strcasecmp(a->title, b->title) != 0) continue;
            dup = k;
        }
        if(dup >= 0) {
            /* the better copy stays: DSD over PCM, more bits, a higher rate
             * (Ian, 10 Oct 20:23); equal or unknown, the first in library order */
            if(alb.tr[i].rank > alb.tr[dup].rank) { free(alb.tr[dup].uri); free(alb.tr[dup].title); alb.tr[dup] = alb.tr[i]; }
            else { free(alb.tr[i].uri); free(alb.tr[i].title); }
            continue;
        }
        if(kept != i) alb.tr[kept] = alb.tr[i];
        kept++;
    }
    if(kept < alb.n) fprintf(stderr, "simpleton-ui: album page: \"%s\": %d of %d tracks are second copies, not shown\n", alb.title, alb.n - kept, alb.n);
    alb.n = kept;
    int last_disc = -1;
    for(int i = 0; i < alb.n; i++) if(alb.tr[i].disc != last_disc) { alb.discs++; last_disc = alb.tr[i].disc; }
    /* an untitled track shows its number (the browser's rule) */
    for(int i = 0; i < alb.n; i++) if(!alb.tr[i].title[0]) {
        char num[16], buf[64];
        snprintf(num, sizeof(num), "%02d", i + 1);
        ui_strf(buf, sizeof(buf), S_TRACK_NUMBERED, "n", num, NULL);
        free(alb.tr[i].title);
        alb.tr[i].title = strdup(buf);
    }
    opened = alb.n > 0;
    sel = 0;
    playing = -1;
    return opened;
}

const char * albumpage_folder(void) { return opened ? alb.folder : NULL; }

/* ---- marks: the playing triangle and the favourite heart ---- */

static lv_obj_t * mark_canvas(lv_obj_t * parent, int w, int h)
{
    if(nmark_bufs >= MARK_BUFS) return NULL;
    lv_draw_buf_t * buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
    if(!buf) return NULL;
    mark_bufs[nmark_bufs++] = buf;
    lv_obj_t * c = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(c, buf);
    lv_canvas_fill_bg(c, lv_color_black(), LV_OPA_TRANSP);
    return c;
}

static void marks_free(void)
{
    for(int i = 0; i < nmark_bufs; i++) lv_draw_buf_destroy(mark_bufs[i]);
    nmark_bufs = 0;
}

/* A triangle pointing right, `r` tall (half-height), in a canvas 2r x 2r. */
static lv_obj_t * make_triangle(lv_obj_t * parent, int r, uint32_t colour)
{
    int s = 2 * r + 2;
    lv_obj_t * c = mark_canvas(parent, s, s);
    if(!c) return NULL;
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(colour);
    d.opa = LV_OPA_COVER;
    d.p[0].x = r - r * 8 / 10; d.p[0].y = 1;
    d.p[1].x = r - r * 8 / 10; d.p[1].y = 2 * r + 1;
    d.p[2].x = r + r * 9 / 10; d.p[2].y = r + 1;
    lv_draw_triangle(&layer, &d);
    lv_canvas_finish_layer(c, &layer);
    return c;
}

/* A heart `r` wide (half-width): two discs and a point, in 2r x 2r. */
static lv_obj_t * make_heart(lv_obj_t * parent, int r, uint32_t colour)
{
    int s = 2 * r + 2;
    lv_obj_t * c = mark_canvas(parent, s, s);
    if(!c) return NULL;
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_rect_dsc_t rc;
    lv_draw_rect_dsc_init(&rc);
    rc.bg_color = lv_color_hex(colour);
    rc.bg_opa = LV_OPA_COVER;
    rc.radius = LV_RADIUS_CIRCLE;
    int cx = r + 1, cy = r + 1;
    int rad = r / 2;                                   /* each lobe */
    lv_area_t a1 = { cx - r, cy - r * 9 / 10, cx, cy - r * 9 / 10 + 2 * rad };
    lv_area_t a2 = { cx, cy - r * 9 / 10, cx + r, cy - r * 9 / 10 + 2 * rad };
    lv_draw_rect(&layer, &rc, &a1);
    lv_draw_rect(&layer, &rc, &a2);
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.color = lv_color_hex(colour);
    d.opa = LV_OPA_COVER;
    d.p[0].x = cx - r * 98 / 100; d.p[0].y = cy - r * 2 / 10;
    d.p[1].x = cx + r * 98 / 100; d.p[1].y = cy - r * 2 / 10;
    d.p[2].x = cx;                d.p[2].y = cy + r;
    lv_draw_triangle(&layer, &d);
    lv_canvas_finish_layer(c, &layer);
    return c;
}

/* ---- this visit's choices ---- */

static const tile_font_t * cover_face(const char * wanted, int dealt)
{
    int i = tilestyle_font_index(wanted);
    if(i < 0 && dealt != TILE_FONT_NONE) i = dealt;
    return tilestyle_font(i);
}

/* Everything that follows from the layout and the seed: the tile styles,
 * the colours, the list face, the split. */
static void choose_for_layout(void)
{
    /* two tile styles: the no-art square's and the page's own (also the
     * header faces when a layout doesn't name them). The square's is the
     * very tile the grid showed (Ian, 10 Oct: its colours and font carry
     * over), the page's a companion from the same colour set; an album
     * with art deals both. */
    char m[300], b[300];
    bool noart[2] = { true, true };
    int letters[2] = { count_letters(split_bracket(alb.title, m, sizeof(m), b, sizeof(b)) ? m : alb.title), count_letters(alb.artist) };
    tilestyle_deal(seed, 2, 2, noart, letters, tst);
    if(alb.has_tile) {
        tst[0] = alb.tile;
        tilestyle_companion(seed, &alb.tile, &tst[1]);
        if(tst[1].font == tst[0].font) tst[1].font = TILE_FONT_NONE;      /* the artist line in another face */
    }

    /* Colours (Ian, 10 Oct 17:39: the unselected tracks in one of the
     * cover's colourful colours - the vibe; the selected track in the colour
     * nearest black or white - the most legible; the playing track in
     * another colourful one). From the cover's palette when it has one, else
     * from the tile colourway and the theme's palettes. */
    bool from_art = alb.has_art && alb.palette.ndom > 0 && lay->colours != PAGE_COLOURS_TILE;
    art_roles_t r;
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0;
    uint32_t dark = tilestyle_dark_ink();
    if(from_art && palette_roles(&alb.palette, lay->colours == PAGE_COLOURS_PAPER ? PAL_MODE_PAPER : PAL_MODE_DOMINANT,
                                 (float)tilestyle_min_contrast(), cream, dark, 0, &r)) {
        c_bg = r.bg; c_ink = r.ink; c_sel = r.ink;
        c_text = r.text ? r.text : r.ink;
        c_play = r.play;
    }
    else {
        const tile_style_t * page = (alb.has_tile && !tv) ? &tst[0] : &tst[1];   /* the handheld page is the tile itself */
        c_bg = page->bg;
        uint32_t ink = page->ink;
        bool ink_colourful = tilestyle_saturation(ink) >= 0.25 && tilestyle_distance(ink, cream) >= 70 && tilestyle_distance(ink, dark) >= 70;
        if(ink_colourful) {
            /* the tile's coloured ink is the text; the legible extreme is the selected colour */
            c_text = ink;
            c_sel = tilestyle_contrast(c_bg, cream) >= tilestyle_contrast(c_bg, dark) ? cream : dark;
            c_ink = c_sel;
        }
        else {
            c_sel = c_ink = ink;
            uint32_t t2, u2;
            tilestyle_page_accents(seed, c_bg, ink, &t2, &u2);
            c_text = tilestyle_distance(t2, ink) >= 70 ? t2 : ink;
        }
        c_play = 0;
        from_art = false;
    }
    /* the playing colour: a colourful one apart from the text, borrowed
     * from the theme when the cover has none to spare */
    if(!c_play || tilestyle_saturation(c_play) < 0.2 || tilestyle_distance(c_play, c_text) < 100) {
        uint32_t p2, s2;
        tilestyle_page_accents(seed, c_bg, c_text, &p2, &s2);
        if(tilestyle_distance(p2, c_text) >= 100 && tilestyle_distance(p2, c_sel) >= 70) c_play = p2;
        else if(tilestyle_distance(s2, c_text) >= 100 && tilestyle_distance(s2, c_sel) >= 70) c_play = s2;
        else if(!c_play) c_play = c_sel;
    }
    /* the selected colour must differ from the text; when the text is the
     * ink itself (nothing colourful read), the other extreme or the theme's */
    if(tilestyle_distance(c_sel, c_text) < 70) {
        uint32_t other = tilestyle_contrast(c_bg, cream) >= tilestyle_contrast(c_bg, dark) ? cream : dark;
        if(tilestyle_distance(other, c_text) >= 70 && tilestyle_contrast(c_bg, other) >= 2.5) c_sel = other;
        else {
            uint32_t p2, s2;
            tilestyle_page_accents(seed, c_bg, c_text, &p2, &s2);
            c_sel = tilestyle_distance(p2, c_play) >= 70 ? p2 : s2;
        }
    }

    /* the list face */
    int li = tilestyle_liner_index(lay->liner);
    if(li < 0) li = tilestyle_liner_pick(seed);
    const tile_font_t * lf = tilestyle_liner(li);
    liner_file = lf ? lf->file : NULL;
    liner_caps = lf ? lf->caps : false;
    liner_leading = lay->leading ? lay->leading : lf ? lf->leading : 100;

    /* the split */
    split = alb.n;
    if(tv && lay->art == PAGE_ART_CENTRE && alb.n > 1) {
        if(alb.discs == 2) { for(int i = 1; i < alb.n; i++) if(alb.tr[i].disc != alb.tr[0].disc) { split = i; break; } }
        else split = alb.n - alb.n / 2;
    }

    /* an album without art: the cassette and record colours from the tile
     * (Ian, 10 Oct 20:49: "start with the same background colour") - the
     * background the tile's, the label a colour of its set the ink reads
     * on, the shell another, a record's disc the shell colour (black, the
     * set's darkest, one visit in five; Ian, 11 Oct); the album line in the
     * tile's face. Now-playing's cassette for a track of this
     * album takes the same (placeholder_prefer), on either screen. The TV
     * square shows one of the tile, the cassette and the record each visit,
     * never the same twice running. */
    square_design = NULL;
    if(!alb.has_art) {
        uint32_t label, shell, dark;
        tilestyle_picture_colours(seed, &tst[0], &label, &shell, &dark);
        placeholder_tint_t tints[8] = { {"background", tst[0].bg}, {"label", label}, {"shell", shell}, {"sleeve", shell},
                                        {"centre", label}, {"disc", dark}, {"groove", dark}, {"spindle", label} };
        memcpy(sq_tints, tints, sizeof(tints));
        sq_ntints = 8;
        sq_ink = tst[0].ink;
        const tile_font_t * f = tst[0].font != TILE_FONT_NONE ? tilestyle_font(tst[0].font) : NULL;
        snprintf(sq_font, sizeof(sq_font), "%s", f ? f->file : "");
        sq_caps = f ? f->caps : false;
        const char ** uris = calloc((size_t)alb.n, sizeof(char *));
        if(uris) for(int i = 0; i < alb.n; i++) uris[i] = alb.tr[i].uri;
        placeholder_prefer(uris, uris ? alb.n : 0, alb.artist, alb.title, sq_tints, sq_ntints, tst[0].bg, sq_ink, sq_font[0] ? sq_font : NULL, sq_caps);
        free(uris);
        if(tv) {
            const char * opt[3];
            int nopt = 0;
            opt[nopt++] = NULL;
            if(placeholder_has_design(placeholder_main_design())) opt[nopt++] = placeholder_main_design();
            if(placeholder_has_design("record")) opt[nopt++] = "record";
            int k = (int)(((seed * 2654435761u) >> 8) % (unsigned)nopt);
            if(nopt > 1 && k == last_square) k = (k + 1) % nopt;
            last_square = k;
            square_design = opt[k];
            if(square_design && strcmp(square_design, "record") == 0) {
                /* Ian, 11 Oct: coloured vinyl most visits - the shell colour,
                 * another of the tile's set - and black about one in five */
                uint32_t x = seed ^ 0x9E3779B9u;            /* mixed, so it doesn't follow the design choice above */
                x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
                bool black = x % 5 == 0;
                sq_tints[5].rgb = sq_tints[6].rgb = black ? dark : shell;
            }
        }
    }
    fprintf(stderr, "simpleton-ui: album page: \"%s\" layout %s, colours %s bg %06x ink %06x text %06x playing %06x selected %06x, liner %s, %d tracks%s%s%s\n",
            alb.title, lay->name, from_art ? (lay->colours == PAGE_COLOURS_PAPER ? "paper" : "art") : "tile",
            c_bg, c_ink, c_text, c_play, c_sel, liner_file ? liner_file : "ui", alb.n, split < alb.n ? " split" : "",
            tv && !alb.has_art ? ", square " : "", tv && !alb.has_art ? (square_design ? square_design : "tile") : "");
    if(!alb.has_art) fprintf(stderr, "simpleton-ui: album page: picture colours background %06x label %06x shell %06x disc %06x ink %06x, album face %s\n",
                             sq_tints[0].rgb, sq_tints[1].rgb, sq_tints[2].rgb, sq_tints[5].rgb, sq_ink, sq_font[0] ? sq_font : "theme");
}

static void choose(void)
{
    tv = ui_screen.kind == UI_SCREEN_TV;
    seed = (unsigned)time(NULL) ^ (unsigned)lv_tick_get() ^ (unsigned)rand();
    lay = tilestyle_layout_pick(seed, tv ? PAGE_SCREEN_TV : PAGE_SCREEN_HANDHELD);
    if(!lay) {
        memset(&lay_default, 0, sizeof(lay_default));
        snprintf(lay_default.name, sizeof(lay_default.name), "default");
        lay_default.screen = tv ? PAGE_SCREEN_TV : PAGE_SCREEN_HANDHELD;
        lay_default.size = 'M';
        lay_default.numbers = PAGE_NUMBERS_PADDED;
        lay = &lay_default;
    }
    choose_for_layout();
}

/* ---- building ---- */

static lv_obj_t * plain(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void page_clear(void)
{
    if(page) { lv_obj_delete(page); page = NULL; }
    marks_free();
    if(frame) { lv_obj_delete(frame); frame = NULL; }
    art_img = field[0] = field[1] = content[0] = content[1] = mark = NULL;
    for(int i = 0; i < alb.n; i++) { alb.tr[i].num = alb.tr[i].lab = alb.tr[i].heart = NULL; }
}

/* A fresh page object, under now-playing's stage when that is hosted here. */
static void page_begin(void)
{
    page_clear();
    page = plain(scr);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, ui_screen.w, ui_screen.h);
    lv_obj_move_background(page);
}

static void art_apply(void)
{
    if(!art_img || !art_px) return;
    lv_image_set_src(art_img, &art_dsc);
    if(!(tv && nowplaying_art_ready())) lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
}

static void art_free(void)
{
    if(art_px) { lv_image_cache_drop(&art_dsc); free(art_px); art_px = NULL; }
}

static void page_art_mode(void);

/* ---- the square's picture, drawn off the UI thread ---- */

typedef struct {
    int      gen;
    char     design[32], font[64], artist[300], album[300], title[300];
    bool     caps;
    uint32_t bg, ink;
    placeholder_tint_t tints[8];
    int      ntints;
} sq_job_t;

static void * sq_thread(void * arg)
{
    sq_job_t * j = arg;
    int w = 0, h = 0;
    uint8_t * px = placeholder_render_design(j->design, j->tints, j->ntints, j->bg, j->ink, j->font[0] ? j->font : NULL, j->caps,
                                             j->artist, j->album, j->title, &w, &h);
    pthread_mutex_lock(&sq_lock);
    if(px && j->gen == sq_gen) {                /* still the latest job */
        free(sq_res.px);
        sq_res.px = px; sq_res.w = w; sq_res.h = h; sq_res.gen = j->gen; sq_res.ready = true;
    }
    else free(px);
    pthread_mutex_unlock(&sq_lock);
    free(j);
    return NULL;
}

/* Draw the square's cassette or record: the names on its label, and the
 * playing track's title on the lower line while one of this album plays. */
static void square_render(void)
{
    if(!square_design) return;
    sq_job_t * j = calloc(1, sizeof(*j));
    if(!j) return;
    pthread_mutex_lock(&sq_lock);
    j->gen = ++sq_gen;
    pthread_mutex_unlock(&sq_lock);
    snprintf(j->design, sizeof(j->design), "%s", square_design);
    snprintf(j->font, sizeof(j->font), "%s", sq_font);
    snprintf(j->artist, sizeof(j->artist), "%s", alb.artist);
    snprintf(j->album, sizeof(j->album), "%s", alb.title);
    if(playing >= 0) snprintf(j->title, sizeof(j->title), "%s", alb.tr[playing].title);
    j->caps = sq_caps;
    j->bg = tst[0].bg;
    j->ink = sq_ink;
    memcpy(j->tints, sq_tints, sizeof(j->tints));
    j->ntints = sq_ntints;
    pthread_t th;
    if(pthread_create(&th, NULL, sq_thread, j) != 0) { free(j); return; }
    pthread_detach(th);
}

/* Drop a picture on its way or waiting (the page is going). */
static void square_drop(void)
{
    pthread_mutex_lock(&sq_lock);
    sq_gen++;
    free(sq_res.px);
    sq_res.px = NULL;
    sq_res.ready = false;
    pthread_mutex_unlock(&sq_lock);
}

/* The playing track changed: a cassette with a title line gets the new one. */
static void square_retitle(void)
{
    if(!tv || !shown || !art_img || !square_design || placeholder_design_lines(square_design) < 3) return;
    square_render();
}

static void poll_cb(lv_timer_t * tm)
{
    (void)tm;
    pthread_mutex_lock(&sq_lock);
    if(sq_res.ready && art_img) {
        uint8_t * px = sq_res.px;
        int w = sq_res.w, h = sq_res.h;
        sq_res.px = NULL;
        sq_res.ready = false;
        pthread_mutex_unlock(&sq_lock);
        art_free();
        art_px = px;
        memset(&art_dsc, 0, sizeof(art_dsc));
        art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        art_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        art_dsc.header.w = (uint32_t)w;
        art_dsc.header.h = (uint32_t)h;
        art_dsc.header.stride = (uint32_t)w * 4;
        art_dsc.data_size = (uint32_t)w * (uint32_t)h * 4;
        art_dsc.data = art_px;
        art_apply();
    }
    else pthread_mutex_unlock(&sq_lock);
    if(tv && art_img && nowplaying_embedded()) {
        /* now-playing's own cover arrives on its own time: ours gives way then */
        bool np_up = nowplaying_art_ready();
        bool ours_hidden = lv_obj_has_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        if(np_up && !ours_hidden) lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        else if(!np_up && ours_hidden && art_px) lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN);
    }
    thumb_res_t r;
    while(thumbs_poll(&r)) {
        if(r.gen != gen || !r.pixels) { free(r.pixels); continue; }
        art_free();
        art_px = r.pixels;
        memset(&art_dsc, 0, sizeof(art_dsc));
        art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        art_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
        art_dsc.header.w = (uint32_t)r.w;
        art_dsc.header.h = (uint32_t)r.h;
        art_dsc.header.stride = (uint32_t)r.w * 4;
        art_dsc.data_size = (uint32_t)r.w * (uint32_t)r.h * 4;
        art_dsc.data = art_px;
        art_apply();
    }
}

/* The cover square on the TV: the art (its preview colour until the
 * thumbnail is decoded), or the no-art tile. */
static void build_square(int ax)
{
    int side = ui_screen.stage;
    lv_obj_t * sq = plain(page);
    lv_obj_set_pos(sq, ax, ui_screen.stage_y);
    lv_obj_set_size(sq, side, side);
    if(alb.has_art) {
        lv_obj_set_style_bg_color(sq, lv_color_hex(alb.colour ? alb.colour : c_bg), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        art_img = lv_image_create(sq);
        lv_obj_set_size(art_img, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(art_img, 0, 0);
        lv_image_set_inner_align(art_img, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
        if(art_px) art_apply();
        else {
            thumb_req_t req = { .id = 0, .gen = ++gen, .px = lib_thumb_px[LIB_THUMB_COUNT - 1] };
            snprintf(req.path, sizeof(req.path), "%s", alb.thumb);
            thumbs_want(&req, 1);
        }
    }
    else {
        lv_obj_set_style_bg_color(sq, lv_color_hex(tst[0].bg), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(sq, lv_color_hex(tst[0].ink), 0);
        if(square_design) {
            /* the cassette or record, in the tile's colours */
            art_img = lv_image_create(sq);
            lv_obj_set_size(art_img, LV_PCT(100), LV_PCT(100));
            lv_obj_set_pos(art_img, 0, 0);
            lv_image_set_inner_align(art_img, LV_IMAGE_ALIGN_STRETCH);
            lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN);
            if(art_px) art_apply();
            else square_render();
        }
        else {
            tiletext_t tt;
            tiletext_make(sq, side, alb.title, alb.artist, &tst[0], &tt);
        }
    }
}

/* One name of the handheld header, fitted into w; returns its height. */
static int build_name(const char * text_in, const tile_font_t * face, int start_px, int maxh, int max_lines,
                      bool centred, int x, int y, int w)
{
    char text[400];
    snprintf(text, sizeof(text), "%s", text_in);
    const char * file = face ? face->file : NULL;
    int leading = face ? face->leading : 100;
    if(face && face->caps) caps_inplace(text);
    int lad[LADDER_STEPS];
    ladder_fill(lad, start_px);
    fit_t ft = { 0 };
    int k = fit_rung(file, leading, lad, 0, LADDER_STEPS - 1, text, w, maxh, max_lines, &ft);
    if(k < 0) {
        /* nothing whole: the smallest size, LVGL wrapping, dots */
        const lv_font_t * f = face_px(file, lad[LADDER_STEPS - 1]);
        ft.font = f; ft.px = lad[LADDER_STEPS - 1]; ft.space = leading_space(f, leading); ft.whole = false;
        ft.lines = max_lines; ft.h = lines_height(f, max_lines, ft.space);
        if(ft.h > maxh) { ft.lines = 1; ft.h = lv_font_get_line_height(f); }
        snprintf(ft.text, sizeof(ft.text), "%s", text);
    }
    lv_obj_t * l = lv_label_create(page);
    lv_obj_set_style_text_font(l, ft.font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c_ink), 0);
    lv_obj_set_style_text_align(l, centred ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(l, ft.space, 0);
    lv_label_set_long_mode(l, ft.whole ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, ft.h);
    lv_label_set_text(l, ft.text);
    return ft.h;
}

/* The handheld's names, the heading above the list (Ian, 10 Oct 18:20: one
 * page, nothing changes size): one name big - the layout's choice, fitted
 * down the ladder into three lines and two fifths of the stage - the other
 * small; returns the y the list starts at. */
static int build_header(int x, int y, int w)
{
    const tile_font_t * fa = cover_face(lay->artist, tst[1].font);
    const tile_font_t * fb = alb.has_tile ? cover_face("", tst[0].font) : cover_face(lay->album, tst[0].font);   /* the tile's face carries over */
    if(alb.has_tile && fa == fb) fa = NULL;                     /* the UI face, then */
    bool centred = lay->header == PAGE_HEADER_CENTRED;
    struct { const char * text; const tile_font_t * face; bool big; } line[2];
    switch(lay->header) {
        case PAGE_HEADER_ALBUM_BIG: line[0] = (typeof(line[0])){ alb.artist, fa, false }; line[1] = (typeof(line[0])){ alb.title, fb, true }; break;
        case PAGE_HEADER_CENTRED:   line[0] = (typeof(line[0])){ alb.title, fb, true };   line[1] = (typeof(line[0])){ alb.artist, fa, false }; break;
        case PAGE_HEADER_NONE:      line[0] = (typeof(line[0])){ alb.artist, fa, false }; line[1] = (typeof(line[0])){ alb.title, fb, false }; break;
        default:                    line[0] = (typeof(line[0])){ alb.artist, fa, true };  line[1] = (typeof(line[0])){ alb.title, fb, false }; break;
    }
    int big_px = lay->header == PAGE_HEADER_ARTIST_BIG ? PX(110) : lay->header == PAGE_HEADER_ALBUM_BIG ? PX(100) : PX(88);
    int gap = PX(12);
    for(int k = 0; k < 2; k++) {
        if(!line[k].text[0]) continue;
        int px, maxh, max_lines;
        if(line[k].big) { px = big_px; maxh = ui_screen.stage * 40 / 100; max_lines = MAX_LINES; }
        else            { px = PX(40); maxh = PX(100); max_lines = 2; }
        int h = build_name(line[k].text, line[k].face, px, maxh, max_lines, centred, x, y, w);
        y += h + gap;
    }
    return y + PX(6);
}

/* A side label over a split list ("Side A"), in the artist face. */
static void build_side_label(int s, int x, int y, int w)
{
    char text[64];
    const char * word = (lay->sides == PAGE_SIDES_NUMBERS && alb.discs == 2) ? T(S_PAGE_DISC) : T(S_PAGE_SIDE);
    if(lay->sides == PAGE_SIDES_LETTERS) snprintf(text, sizeof(text), "%s %c", word, s ? 'B' : 'A');
    else snprintf(text, sizeof(text), "%s %d", word, s + 1);
    const tile_font_t * fa = cover_face(lay->artist, tst[1].font);
    if(fa && fa->caps) caps_inplace(text);
    const lv_font_t * f = face_px(fa ? fa->file : NULL, tvpx(40));
    lv_obj_t * l = lv_label_create(page);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c_text), 0);
    lv_obj_set_style_text_align(l, lay->align == PAGE_ALIGN_CENTRE ? LV_TEXT_ALIGN_CENTER : lay->align == PAGE_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    lv_label_set_text(l, text);
}

static int side_of(int i) { return i >= split ? 1 : 0; }
static int side_first(int s) { return s ? split : 0; }
static int side_count(int s) { return s ? alb.n - split : split; }

static void number_text(int i, char * out, size_t len)
{
    switch(lay->numbers) {
        case PAGE_NUMBERS_PADDED: snprintf(out, len, "%02d", i + 1); break;
        case PAGE_NUMBERS_DOT:    snprintf(out, len, "%d.", i + 1); break;
        case PAGE_NUMBERS_PLAIN:  snprintf(out, len, "%d", i + 1); break;
        default: out[0] = 0; break;
    }
}

/* Where the first line starts and how wide it is, for the marks. */
static void first_line(const track_t * t, const lv_font_t * f, int w, int * tx, int * tw)
{
    char line[400];
    const char * nl = strchr(t->wrapped, '\n');
    size_t n = nl ? (size_t)(nl - t->wrapped) : strlen(t->wrapped);
    if(n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, t->wrapped, n);
    line[n] = 0;
    *tw = text_w(f, line);
    if(*tw > w) *tw = w;
    *tx = lay->align == PAGE_ALIGN_CENTRE ? (w - *tw) / 2 : lay->align == PAGE_ALIGN_RIGHT ? w - *tw : 0;
}

static void row_style(int i)
{
    track_t * t = &alb.tr[i];
    if(!t->lab) return;
    bool is_sel = i == sel && (!tv || focus == FOCUS_LIST);   /* on the cover no track is highlighted (Ian) */
    bool is_play = i == playing;
    uint32_t col = is_play ? c_play : is_sel ? c_sel : c_text;
    const lv_font_t * f = is_sel ? f_grown : f_list;
    lv_obj_set_style_text_font(t->lab, f, 0);
    lv_obj_set_style_text_line_space(t->lab, lh - lv_font_get_line_height(f), 0);
    lv_obj_set_style_text_color(t->lab, lv_color_hex(col), 0);
    if(t->num) lv_obj_set_style_text_color(t->num, lv_color_hex(col), 0);
    int tx = is_sel ? t->tx1 : t->tx0, tw = is_sel ? t->tw1 : t->tw0;
    int cy = t->y + lh / 2;
    if(t->heart) {
        int r = lsize * 26 / 100;
        lv_obj_set_pos(t->heart, lead + gutter + tx + tw + lsize * 55 / 100 - r - 1, cy - r - 1);
    }
    if(is_play && mark) {
        int r = lsize * 25 / 100;
        char num[16];
        number_text(i, num, sizeof(num));
        int gut = (num[0] && lay->align == PAGE_ALIGN_LEFT) ? lsize / 2 + text_w(f_list, num) : 0;
        lv_obj_set_parent(mark, content[t->side]);
        lv_obj_set_pos(mark, lead + gutter + tx - lsize * 75 / 100 - gut - r - 1, cy - r - 1);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scroll_exec(void * var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, -v); }

static void scroll_side(int s, int y, bool anim)
{
    int max = total_h[s] - col_h;
    if(max < 0) max = 0;
    if(y > max) y = max;
    if(y < 0) y = 0;
    lv_anim_delete(content[s], scroll_exec);
    int from = scroll_y[s];
    scroll_y[s] = y;
    if(!anim || from == y) { lv_obj_set_y(content[s], -y); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, content[s]);
    lv_anim_set_exec_cb(&a, scroll_exec);
    lv_anim_set_values(&a, from, y);
    lv_anim_set_duration(&a, SCROLL_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Keep the selected track in view (a row's gap above / below it). */
static void ensure_visible(bool anim)
{
    if(alb.n == 0) return;
    const track_t * t = &alb.tr[sel];
    int s = t->side;
    int top = t->y - row_gap, bot = t->y + t->h + row_gap;
    int y = scroll_y[s];
    if(top < y) y = top;
    else if(bot > y + col_h) y = bot - col_h;
    scroll_side(s, y, anim);
}

static void build_list(void)
{
    char num[16];
    number_text(0, num, sizeof(num));
    gutter = num[0] ? lsize * 160 / 100 : 0;
    lead = lsize * 2;
    for(int s = 0; s < 2; s++) {
        if(side_count(s) <= 0) continue;
        field[s] = plain(page);
        lv_obj_set_pos(field[s], col_x[s] - gutter - lead, col_y);
        lv_obj_set_size(field[s], col_w[s] + gutter + lead, col_h);
        content[s] = plain(field[s]);
        lv_obj_set_pos(content[s], 0, 0);
        int y = 0;
        for(int i = side_first(s); i < side_first(s) + side_count(s); i++) {
            track_t * t = &alb.tr[i];
            t->side = s;
            char title[400];
            snprintf(title, sizeof(title), "%s", t->title);
            if(liner_caps) caps_inplace(title);
            bool whole;
            int lines = wrap_words(f_grown, title, col_w[s], t->wrapped, sizeof(t->wrapped), &whole);
            if(lines > MAX_LINES) lines = MAX_LINES;
            t->lines = lines;
            t->y = y;
            t->h = lines * lh;
            number_text(i, num, sizeof(num));
            if(num[0]) {
                t->num = lv_label_create(content[s]);
                lv_obj_set_style_text_font(t->num, f_list, 0);
                lv_obj_set_style_text_align(t->num, LV_TEXT_ALIGN_RIGHT, 0);
                lv_label_set_long_mode(t->num, LV_LABEL_LONG_MODE_CLIP);
                lv_obj_set_pos(t->num, 0, y);
                lv_obj_set_size(t->num, lead + gutter - lsize / 2, lh);
                lv_label_set_text(t->num, num);
            }
            t->lab = lv_label_create(content[s]);
            lv_obj_set_style_text_align(t->lab, lay->align == PAGE_ALIGN_CENTRE ? LV_TEXT_ALIGN_CENTER : lay->align == PAGE_ALIGN_RIGHT ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_long_mode(t->lab, whole && lines <= MAX_LINES ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_pos(t->lab, lead + gutter, y);
            lv_obj_set_size(t->lab, col_w[s], t->h);
            lv_label_set_text(t->lab, whole ? t->wrapped : title);
            first_line(t, f_list, col_w[s], &t->tx0, &t->tw0);
            first_line(t, f_grown, col_w[s], &t->tx1, &t->tw1);
            if(t->fav) t->heart = make_heart(content[s], lsize * 26 / 100, FAV_RED);
            y += t->h + row_gap;
        }
        total_h[s] = y - row_gap;
        lv_obj_set_size(content[s], col_w[s] + gutter + lead, total_h[s] > col_h ? total_h[s] : col_h);
        scroll_y[s] = 0;
    }
    mark = make_triangle(content[side_of(playing >= 0 ? playing : 0)] ? content[side_of(playing >= 0 ? playing : 0)] : scr, lsize * 25 / 100, c_play);
    if(mark) lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    for(int i = 0; i < alb.n; i++) row_style(i);
    ensure_visible(false);
}

static void apply_focus(void);
static void np_host(void);

static void build(void)
{
    page_begin();
    lv_obj_set_style_bg_color(scr, lv_color_hex(c_bg), 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(c_ink), 0);

    /* list type: S / M / L */
    int base_tv = lay->size == 'S' ? 34 : lay->size == 'L' ? 44 : 38;
    int base_hh = lay->size == 'S' ? 32 : lay->size == 'L' ? 40 : 36;
    lsize = sz(base_tv, base_hh);
    f_list = face_px(liner_file, lsize);
    f_grown = face_px(liner_file, lsize * GROW_PCT / 100);
    lh = lv_font_get_line_height(f_grown) + leading_space(f_grown, liner_leading);
    row_gap = lsize * 45 / 100;
    char num[16];
    number_text(0, num, sizeof(num));
    int gut = num[0] ? lsize * 160 / 100 : 0;

    if(tv) {
        int stage = ui_screen.stage, rest = ui_screen.w - stage;
        int ax = lay->art == PAGE_ART_LEFT ? 0 : lay->art == PAGE_ART_RIGHT ? rest : rest / 2;
        sq_x = ax;
        build_square(ax);
        col_y = ui_screen.stage_y + tvpx(80);
        col_h = stage - tvpx(80) - tvpx(40);
        if(lay->art == PAGE_ART_CENTRE) {
            int half = rest / 2, margin = tvpx(50);
            for(int s = 0; s < 2; s++) {
                int x0 = s ? half + stage : 0;
                col_x[s] = x0 + margin + gut;
                col_w[s] = half - 2 * margin - gut;
            }
            if(split < alb.n && lay->sides != PAGE_SIDES_NONE) {
                col_y = ui_screen.stage_y + tvpx(130);
                col_h = stage - tvpx(130) - tvpx(40);
                for(int s = 0; s < 2; s++) build_side_label(s, col_x[s], ui_screen.stage_y + tvpx(60), col_w[s]);
            }
        }
        else {
            int x0 = lay->art == PAGE_ART_LEFT ? stage : 0, margin = tvpx(70);
            col_x[0] = x0 + margin + gut;
            col_w[0] = rest - 2 * margin - gut;
            col_x[1] = col_x[0]; col_w[1] = col_w[0];
        }
    }
    else {
        int margin = PX(50);
        col_x[0] = ui_screen.stage_x + margin + gut;
        col_w[0] = ui_screen.stage - 2 * margin - gut;
        col_x[1] = col_x[0]; col_w[1] = col_w[0];
        int y = build_header(col_x[0], ui_screen.stage_y + PX(40), col_w[0]);
        col_y = y;
        col_h = ui_screen.stage_y + ui_screen.stage - PX(20) - y;
        if(col_h < lh) col_h = lh;
    }
    build_list();
    if(tv) {
        /* the cover's frame, above now-playing's stage when that is hosted here */
        frame = plain(scr);
        lv_obj_set_pos(frame, sq_x, ui_screen.stage_y);
        lv_obj_set_size(frame, ui_screen.stage, ui_screen.stage);
        lv_obj_set_style_border_color(frame, lv_color_hex(c_play), 0);   /* the playing colour: the selected colour can sink into the art (Ian) */
        lv_obj_set_style_border_width(frame, tvpx(FRAME_W), 0);
        lv_obj_set_style_border_opa(frame, LV_OPA_COVER, 0);
        lv_obj_move_foreground(frame);
        apply_focus();
        np_host();                                /* now-playing's stage over the square, under the frame */
    }
}

static void apply_focus(void)
{
    if(!frame) return;
    if(focus == FOCUS_ART && !art_paged) lv_obj_remove_flag(frame, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(frame, LV_OBJ_FLAG_HIDDEN);
}

/* The page's own picture gives way when now-playing shows its (the full
 * cover of the playing track, this album's); behind a cover that isn't
 * square now-playing paints the page's colour, not the page's picture. */
static bool album_has_uri(const char * uri)
{
    if(!uri || !uri[0]) return false;
    for(int i = 0; i < alb.n; i++) if(strcmp(alb.tr[i].uri, uri) == 0) return true;
    return false;
}

static void page_art_mode(void)
{
    if(!tv) return;
    /* now-playing's art may stay up across a track change within this album
     * (same picture); art of another album stays hidden until the new
     * track's is decoded - no flash of the previous cover (Ian) */
    /* an album without art keeps its own picture: now-playing's cassette
     * never comes over it (Ian, 10 Oct 20:49) */
    if(nowplaying_embedded()) nowplaying_embed_art(playing >= 0 && alb.has_art, c_bg, album_has_uri(nowplaying_art_file()));
    bool np_up = nowplaying_art_ready();            /* its picture is actually up: ours gives way */
    if(art_img) { if(np_up) lv_obj_add_flag(art_img, LV_OBJ_FLAG_HIDDEN); else if(art_px) lv_obj_remove_flag(art_img, LV_OBJ_FLAG_HIDDEN); }
}

/* TV: now-playing lives in the cover square while the page is up. Its own
 * art shows there when the playing track is this album's (the full cover,
 * pageable); otherwise the page's picture stays and only the controls
 * come over it. */
static void np_host(void)
{
    if(!tv || !shown) return;
    nowplaying_embed(scr, sq_x, ui_screen.stage_y, playing >= 0 && alb.has_art, c_bg);
    if(frame) lv_obj_move_foreground(frame);
    page_art_mode();
}

/* ---- the screen ---- */

void albumpage_create(void (*on_exit)(void), void (*on_play)(void))
{
    exit_cb = on_exit;
    play_cb = on_play;
    thumbs_init();
    char theme_path[512];
    config_theme_file(theme_path, sizeof(theme_path));
    tilestyle_load(theme_path);
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    poll_timer = lv_timer_create(poll_cb, 40, NULL);
    lv_timer_pause(poll_timer);
    playing = -1;
}

/* `started`: a different song than last time, and playing. */
static void find_playing(bool * started)
{
    int was = playing;
    playing = -1;
    mpd_status_t st;
    bool got = mpd_status(&st);
    bool play = got && st.file[0] && strcmp(st.state, "stop") != 0;
    if(play) for(int i = 0; i < alb.n; i++) if(strcmp(alb.tr[i].uri, st.file) == 0) { playing = i; break; }
    if(play && playing < 0 && alb.n > 0)
        fprintf(stderr, "simpleton-ui: album page: playing \"%s\" is not a track of this page (first track \"%s\")\n", st.file, alb.tr[0].uri);
    if(!got) fprintf(stderr, "simpleton-ui: album page: no MPD status\n");
    if(started) *started = play && strcmp(st.file, last_song) != 0;
    snprintf(last_song, sizeof(last_song), "%s", got ? st.file : "");
    if(tv && got) nowplaying_expect_file(st.file);
    if(!shown) return;
    if(was != playing) {
        if(mark && playing < 0) lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
        if(was >= 0) row_style(was);
        if(playing >= 0) row_style(playing);
        square_retitle();
    }
    page_art_mode();
}

void albumpage_show(bool fresh)
{
    if(!opened) { if(exit_cb) exit_cb(); return; }
    shown = true;
    tv = ui_screen.kind == UI_SCREEN_TV;
    if(fresh || !lay) {
        sel = 0;
        choose();
    }
    focus = FOCUS_LIST;
    art_paged = false;
    lv_screen_load(scr);
    lv_timer_resume(poll_timer);
    playing = -1;
    last_song[0] = 0;
    find_playing(NULL);
    build();
}

void albumpage_hide(void)
{
    shown = false;
    focus = FOCUS_LIST;
    nowplaying_unembed();
    lv_timer_pause(poll_timer);
    thumbs_want(NULL, 0);
    square_drop();
    page_clear();
    art_free();
}

static void play_from(int index)
{
    char ** uris = calloc((size_t)alb.n, sizeof(char *));
    if(!uris) return;
    for(int i = 0; i < alb.n; i++) uris[i] = alb.tr[i].uri;
    bool ok = mpd_play_uris(uris, alb.n, index);
    free(uris);
    if(!ok) return;
    playing = index;
    for(int i = 0; i < alb.n; i++) row_style(i);
    snprintf(last_song, sizeof(last_song), "%s", alb.tr[index].uri);
    square_retitle();
    if(tv) { nowplaying_expect_file(alb.tr[index].uri); page_art_mode(); nowplaying_poke(); }    /* the controls pop up over the cover and fade; the list keeps the D-pad */
    else if(play_cb) play_cb();                                    /* the handheld goes to now-playing, as the browser does */
}

static void set_sel(int i)
{
    if(i < 0) i = 0;
    if(i > alb.n - 1) i = alb.n - 1;
    int old = sel;
    sel = i;
    row_style(old);
    row_style(sel);
    ensure_visible(true);
}

/* TV: Left/Right from the list towards the cover select it; from the
 * cover, on towards the other side of a split list, or back. */
static bool art_is_towards(ui_action_t a, int side)
{
    switch(lay->art) {
        case PAGE_ART_LEFT:  return a == ACT_LEFT;
        case PAGE_ART_RIGHT: return a == ACT_RIGHT;
        default:             return (a == ACT_RIGHT && side == 0) || (a == ACT_LEFT && side == 1);
    }
}

static void set_focus(focus_t f)
{
    if(f == focus) return;
    focus = f;
    art_paged = false;
    apply_focus();
    row_style(sel);
}

static void tv_handle(ui_action_t a)
{
    /* the controls are up: Back takes them down from anywhere; on the
     * cover they have the D-pad */
    if(nowplaying_controls_visible()) {
        if(a == ACT_BACK) { nowplaying_hide_controls(); return; }
        if(focus == FOCUS_ART) { nowplaying_handle_action(a); return; }
    }
    if(focus == FOCUS_ART) {
        switch(a) {
            case ACT_UP:
            case ACT_DOWN:
                nowplaying_poke();                    /* the controls come up; from now on they have the D-pad */
                return;
            case ACT_SELECT:
                if(playing >= 0 && alb.has_art) {     /* the cover shown is now-playing's: turn its pages */
                    art_paged = true;
                    apply_focus();
                    nowplaying_browse();
                }
                return;
            case ACT_LEFT:
            case ACT_RIGHT:
                if(lay->art == PAGE_ART_CENTRE && split < alb.n) {
                    /* the side the press points to, the same row */
                    int s = side_of(sel), row = sel - side_first(s);
                    int o = a == ACT_RIGHT ? 1 : 0;
                    set_focus(FOCUS_LIST);
                    if(o != s) { int t = side_first(o) + row; if(t > side_first(o) + side_count(o) - 1) t = side_first(o) + side_count(o) - 1; set_sel(t); }
                }
                else set_focus(FOCUS_LIST);
                return;
            case ACT_BACK:
                set_focus(FOCUS_LIST);
                return;
            default: return;
        }
    }
    switch(a) {
        case ACT_UP:   set_sel(sel - 1); return;
        case ACT_DOWN: set_sel(sel + 1); return;
        case ACT_LEFT:
        case ACT_RIGHT:
            if(art_is_towards(a, side_of(sel))) set_focus(FOCUS_ART);
            return;
        case ACT_SELECT: play_from(sel); return;
        case ACT_BACK:   if(exit_cb) exit_cb(); return;
        default: return;
    }
}

void albumpage_handle_action(ui_action_t a)
{
    if(!shown || alb.n == 0) return;
    if(tv) { tv_handle(a); return; }
    switch(a) {
        case ACT_DOWN:   set_sel(sel + 1); return;
        case ACT_UP:     set_sel(sel - 1); return;
        case ACT_SELECT: play_from(sel); return;
        case ACT_BACK:   if(exit_cb) exit_cb(); return;
        default:         return;
    }
}

void albumpage_mpd_changed(void)
{
    if(!shown) return;
    bool started = false;
    find_playing(&started);
    if(started && tv) nowplaying_poke();      /* a track started: the controls pop up over the cover and fade */
}
