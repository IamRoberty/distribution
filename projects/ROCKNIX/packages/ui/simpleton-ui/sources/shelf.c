/*
 * SimpletonOS UI - the tape shelf view style (implementation). See shelf.h.
 *
 * Geometry, in one place. S is the scale from the 1080 master to this
 * screen: the shelf fills the screen's height (spines stand), the stack
 * fills its width (tapes lie), so S = (that side) / 1080. With the master
 * numbers rest 0.889 and rail 60: 60 + 960 + 60 = 1080, which is why the
 * resting tapes sit exactly between the rails at every size.
 *
 *   step        the slot pitch = a resting tape's thickness (160 at 1080)
 *   slot k      k = 0 is the centre; the shelf shows k = -K..K
 *   offset      the slide: pixels the ring is displaced during a move
 *
 * Composition. Every item gets two images: the resting spine and the
 * pulled-out one (both downscaled from the master with an area filter, so
 * they stay sharp), each with the label written on at the size that
 * rendition asks for. The label area is found on the master by walking out
 * from the tile's centre while the colour stays flat, which is how the
 * tiles are drawn (a flat label with the text left out). A tile that can't
 * be read becomes a flat tape in the back colour, so a missing file costs
 * a plain tape, never the page.
 *
 * Text is drawn with LVGL's own label drawing (FreeType through fonts.c,
 * with bidi), onto a small canvas, then blended onto the spine - turned a
 * quarter turn for the shelf, which is lossless. A display face is used on
 * its own when it has every letter of the name, and with the Noto chain
 * behind it otherwise, so a Russian or Thai tape name still appears.
 */
#include "shelf.h"
#include "config.h"
#include "theme.h"

#include <ctype.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MASTER_H      1080
#define PT_TO_PX      (4.0 / 3.0)        /* Ian's Krita document: points at 96 ppi */
#define SHRINK_MIN    0.55               /* text shrinks this far before "..." (as the placeholder label does) */
#define SLIDE_MS      150
#define POP_MS        120
#define MAX_ITEMS     64
#define LABEL_TOL     64                 /* colour distance (sum of channel differences) that still counts as the label */
#define LABEL_RUN     4                  /* this many off-colour pixels in a row end the label */

typedef struct { uint32_t * px; int w, h; } argb_t;       /* 0xAARRGGBB, straight alpha */

typedef struct {
    shelf_item_t   spec;
    char           label[256];
    argb_t         rest, pull;
    lv_image_dsc_t rest_dsc, pull_dsc;
} tape_t;

typedef struct {
    lv_obj_t *     obj;              /* the view (clips)                      */
    shelf_style_t  st;
    bool           wide;             /* bookshelf (true) or stack              */
    double         S;                /* master -> screen                        */
    int            step;             /* slot pitch, px                          */
    int            rest_w, rest_h;   /* resting spine on screen (unrotated: w = thickness, h = length) */
    int            pull_w, pull_h;
    int            rail;             /* rail thickness, px                      */
    int            K;                /* slots each side of the centre           */
    tape_t         tapes[MAX_ITEMS];
    bool           hidden[MAX_ITEMS];
    int            vis[MAX_ITEMS];   /* the ring: indexes of the visible items, in order */
    int            nvis;
    int            count;
    int            sel;              /* an item index, always visible         */
    int            offset;           /* slide displacement, px                  */
    lv_obj_t *     back;
    lv_obj_t *     rail_a, * rail_b; /* wood: top/bottom or left/right           */
    lv_obj_t **    slots;            /* 2K+1 images                              */
    lv_obj_t *     pulled;           /* the selected tape, in front               */
    argb_t         wood_a, wood_b;
    lv_image_dsc_t wood_a_dsc, wood_b_dsc;
    int            wood_off;         /* running offset of the grain, px           */
    lv_anim_t      anim;
} shelf_t;

/* ------------------------------------------------------------ images */

static void argb_free(argb_t * a) { free(a->px); a->px = NULL; a->w = a->h = 0; }

static bool argb_alloc(argb_t * a, int w, int h)
{
    a->w = w; a->h = h;
    a->px = calloc((size_t)w * (size_t)h, 4);
    return a->px != NULL;
}

static void png_quiet(png_structp p, png_const_charp m) { (void)p; (void)m; }   /* Krita's ICC profiles would warn on every load */
static void png_fail(png_structp p, png_const_charp m) { (void)m; longjmp(png_jmpbuf(p), 1); }

/* A PNG as 0xAARRGGBB. Any depth, palette, gray or RGB; alpha kept, else 255. */
static bool load_png(const char * path, argb_t * out)
{
    memset(out, 0, sizeof(*out));
    FILE * f = fopen(path, "rb");
    if(!f) return false;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_fail, png_quiet);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if(!info) { if(png) png_destroy_read_struct(&png, NULL, NULL); fclose(f); return false; }
    png_bytep * rows = NULL;
    if(setjmp(png_jmpbuf(png))) {
        free(rows); argb_free(out);
        png_destroy_read_struct(&png, &info, NULL); fclose(f);
        return false;
    }
    png_init_io(png, f);
    png_read_info(png, info);
    png_uint_32 w, h;
    int depth, ctype;
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, NULL, NULL, NULL);
    if(w == 0 || h == 0 || w > 8192 || h > 8192) longjmp(png_jmpbuf(png), 1);
    if(ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if(ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if(depth == 16) png_set_strip_16(png);
    if(ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);     /* RGB -> RGBA; no-op when alpha is there */
    png_set_bgr(png);                                  /* bytes B,G,R,A = little-endian 0xAARRGGBB */
    png_read_update_info(png, info);
    if(png_get_rowbytes(png, info) != w * 4) longjmp(png_jmpbuf(png), 1);
    if(!argb_alloc(out, (int)w, (int)h)) longjmp(png_jmpbuf(png), 1);
    rows = malloc(sizeof(png_bytep) * h);
    if(!rows) longjmp(png_jmpbuf(png), 1);
    for(png_uint_32 y = 0; y < h; y++) rows[y] = (png_bytep)(out->px + (size_t)y * w);
    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return true;
}

/* Area-average resample (the filter we use everywhere for shrinking). Works
 * for enlarging too (then it is nearest-box), which the shelf never asks for. */
static bool scale_to(const argb_t * src, int dw, int dh, argb_t * dst)
{
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
    if(!argb_alloc(dst, dw, dh)) return false;
    for(int y = 0; y < dh; y++) {
        int sy0 = (int)((long)y * src->h / dh), sy1 = (int)((long)(y + 1) * src->h / dh);
        if(sy1 <= sy0) sy1 = sy0 + 1;
        if(sy1 > src->h) sy1 = src->h;
        for(int x = 0; x < dw; x++) {
            int sx0 = (int)((long)x * src->w / dw), sx1 = (int)((long)(x + 1) * src->w / dw);
            if(sx1 <= sx0) sx1 = sx0 + 1;
            if(sx1 > src->w) sx1 = src->w;
            unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
            for(int sy = sy0; sy < sy1; sy++) {
                const uint32_t * row = src->px + (size_t)sy * src->w;
                for(int sx = sx0; sx < sx1; sx++) {
                    uint32_t p = row[sx];
                    a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
                }
            }
            dst->px[(size_t)y * dw + x] = ((a / n) << 24) | ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return true;
}

/* A quarter turn: clockwise (the top edge becomes the right edge) or
 * counter-clockwise (the top edge becomes the left edge). */
static bool rotate(const argb_t * src, bool clockwise, argb_t * dst)
{
    if(!argb_alloc(dst, src->h, src->w)) return false;
    for(int y = 0; y < src->h; y++) {
        const uint32_t * row = src->px + (size_t)y * src->w;
        for(int x = 0; x < src->w; x++) {
            int nx, ny;
            if(clockwise) { nx = src->h - 1 - y; ny = x; }
            else          { nx = y;              ny = src->w - 1 - x; }
            dst->px[(size_t)ny * dst->w + nx] = row[x];
        }
    }
    return true;
}

/* `src` over `dst` at (x, y), straight alpha; clipped. */
static void blend(argb_t * dst, const argb_t * src, int x0, int y0)
{
    for(int y = 0; y < src->h; y++) {
        int dy = y0 + y;
        if(dy < 0 || dy >= dst->h) continue;
        for(int x = 0; x < src->w; x++) {
            int dx = x0 + x;
            if(dx < 0 || dx >= dst->w) continue;
            uint32_t s = src->px[(size_t)y * src->w + x];
            unsigned a = s >> 24;
            if(a == 0) continue;
            uint32_t * d = dst->px + (size_t)dy * dst->w + dx;
            if(a == 255) { *d = s; continue; }
            uint32_t p = *d;
            unsigned da = p >> 24;
            unsigned oa = a + da * (255 - a) / 255;
            unsigned r = (((s >> 16) & 255) * a + ((p >> 16) & 255) * (255 - a)) / 255;
            unsigned g = (((s >> 8) & 255) * a + ((p >> 8) & 255) * (255 - a)) / 255;
            unsigned b = ((s & 255) * a + (p & 255) * (255 - a)) / 255;
            *d = (oa << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void dsc_wrap(lv_image_dsc_t * d, const argb_t * a)
{
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = (uint32_t)a->w;
    d->header.h = (uint32_t)a->h;
    d->header.stride = (uint32_t)a->w * 4;
    d->data_size = (uint32_t)a->w * (uint32_t)a->h * 4;
    d->data = (const uint8_t *)a->px;
}

/* ------------------------------------------------------------- label */

static int colour_dist(uint32_t a, uint32_t b)
{
    return abs((int)((a >> 16) & 255) - (int)((b >> 16) & 255)) +
           abs((int)((a >> 8) & 255) - (int)((b >> 8) & 255)) +
           abs((int)(a & 255) - (int)(b & 255));
}

/* The flat label area around the tile's centre: walk out along the centre
 * row and column while the colour stays close to the centre's. A painted
 * label has grain, so a stray pixel or two off-colour doesn't end the walk;
 * a run of them (the shell's edge) does. Returns false when the centre
 * isn't a flat area worth writing on. */
static int walk(const argb_t * t, int x, int y, int dx, int dy, uint32_t c)
{
    int last_good_x = x, last_good_y = y, bad = 0;
    for(;;) {
        x += dx; y += dy;
        if(x < 0 || y < 0 || x >= t->w || y >= t->h) break;
        if(colour_dist(t->px[(size_t)y * t->w + x], c) < LABEL_TOL) { last_good_x = x; last_good_y = y; bad = 0; }
        else if(++bad >= LABEL_RUN) break;
    }
    return dx ? last_good_x : last_good_y;
}

static bool label_box(const argb_t * t, int * x0, int * y0, int * x1, int * y1)
{
    int cx = t->w / 2, cy = t->h / 2;
    /* the centre colour: a small average, so one grain pixel doesn't pick the tone */
    unsigned r = 0, g = 0, b = 0, n = 0;
    for(int y = cy - 2; y <= cy + 2; y++) for(int x = cx - 2; x <= cx + 2; x++) {
        uint32_t p = t->px[(size_t)y * t->w + x];
        r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
    }
    uint32_t c = ((r / n) << 16) | ((g / n) << 8) | (b / n);
    *x0 = walk(t, cx, cy, -1, 0, c);
    *x1 = walk(t, cx, cy, +1, 0, c);
    *y0 = walk(t, cx, cy, 0, -1, c);
    *y1 = walk(t, cx, cy, 0, +1, c);
    return (*x1 - *x0) >= 8 && (*y1 - *y0) >= 8;
}

/* ---- letter case (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic;
 * other scripts have no case and pass through) ---- */

static uint32_t utf8_next(const char ** s)
{
    const unsigned char * p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = 0;
    if(c >= 0xF0) { c &= 0x07; n = 3; }
    else if(c >= 0xE0) { c &= 0x0F; n = 2; }
    else if(c >= 0xC0) { c &= 0x1F; n = 1; }
    p++;
    for(int i = 0; i < n && (*p & 0xC0) == 0x80; i++, p++) c = (c << 6) | (*p & 0x3F);
    *s = (const char *)p;
    return c;
}

static int utf8_put(char * out, uint32_t c)
{
    if(c < 0x80) { out[0] = (char)c; return 1; }
    if(c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if(c < 0x10000) { out[0] = (char)(0xE0 | (c >> 12)); out[1] = (char)(0x80 | ((c >> 6) & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F)); out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

static uint32_t to_upper(uint32_t c)
{
    if(c < 128) return (uint32_t)toupper((int)c);
    if(c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;           /* Latin-1 */
    if(c == 0xFF) return 0x178;
    if(c >= 0x100 && c <= 0x17F) return (c & 1) && c != 0x131 ? c - 1 : c;   /* Latin Extended-A pairs */
    if(c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;      /* Greek */
    if(c >= 0x430 && c <= 0x44F) return c - 0x20;                      /* Cyrillic */
    if(c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

static uint32_t to_lower(uint32_t c)
{
    if(c < 128) return (uint32_t)tolower((int)c);
    if(c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if(c == 0x178) return 0xFF;
    if(c >= 0x100 && c <= 0x17F) return !(c & 1) && c != 0x130 ? c + 1 : c;
    if(c >= 0x391 && c <= 0x3A9) return c + 0x20;
    if(c >= 0x410 && c <= 0x42F) return c + 0x20;
    if(c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

static void apply_case(const char * in, shelf_case_t cs, char * out, size_t len)
{
    size_t n = 0;
    bool word_start = true;
    while(*in && n + 5 < len) {
        uint32_t c = utf8_next(&in);
        uint32_t o = c;
        switch(cs) {
            case SHELF_CASE_UPPER: o = to_upper(c); break;
            case SHELF_CASE_LOWER: o = to_lower(c); break;
            case SHELF_CASE_TITLE: o = word_start ? to_upper(c) : to_lower(c); break;
            default: break;
        }
        word_start = (c == ' ' || c == '-' || c == '/');
        n += (size_t)utf8_put(out + n, o);
    }
    out[n] = 0;
}

/* Cut `text` (in place, on a letter boundary) so that with "..." appended
 * it is at most `max_w` wide in `font`. */
static void trim_to(char * text, const lv_font_t * font, int max_w)
{
    static const char dots[] = "\xE2\x80\xA6";
    lv_point_t sz;
    size_t n = strlen(text);
    while(n > 0) {
        /* step back one UTF-8 letter */
        do { n--; } while(n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80);
        char buf[300];
        snprintf(buf, sizeof(buf), "%.*s%s", (int)n, text, dots);
        lv_text_get_size(&sz, buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(sz.x <= max_w) { memcpy(text, buf, strlen(buf) + 1); return; }
    }
    text[0] = 0;
}

/* Draw `text` in `font` and `ink` into a fresh image exactly the text's
 * size (transparent elsewhere). LVGL's label drawing, so bidi and the
 * fallback chain behave exactly as on every other screen. */
static bool render_text(const char * text, const lv_font_t * font, uint32_t ink, argb_t * out)
{
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 4, h = sz.y + 4;
    if(w < 8 || h < 8) return false;
    /* the canvas wants its stride aligned; keep w a multiple of 4 px */
    w = (w + 3) & ~3;
    if(!argb_alloc(out, w, h)) return false;

    lv_obj_t * scratch = lv_obj_create(NULL);              /* an off-screen parent */
    lv_obj_t * canvas = lv_canvas_create(scratch);
    lv_canvas_set_buffer(canvas, out->px, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.color = lv_color_hex(ink);
    d.opa = LV_OPA_COVER;
    lv_area_t a = { 2, 2, w - 3, h - 3 };
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_delete(scratch);
    return true;
}

/* ------------------------------------------------------- composing */

/* One rendition of a tape: the master scaled to `h` tall (keeping its
 * proportions), the label written on, turned on its side for the stack. */
static bool compose(shelf_t * sh, const argb_t * master, const tape_t * t, int h, argb_t * out, bool * fell_back)
{
    int w = (int)lround((double)master->w * h / master->h);
    if(w < 1) w = 1;
    argb_t tile;
    if(!scale_to(master, w, h, &tile)) return false;

    /* the label area, on the master, then on this rendition */
    int bx0, by0, bx1, by1;
    bool has_box = label_box(master, &bx0, &by0, &bx1, &by1);
    double k = (double)h / master->h;
    int lx0 = (int)(bx0 * k), ly0 = (int)(by0 * k), lx1 = (int)(bx1 * k), ly1 = (int)(by1 * k);
    if(!has_box) { lx0 = w / 6; lx1 = w - w / 6; ly0 = h / 12; ly1 = h - h / 12; }

    /* text size for this rendition: pt at the master, scaled */
    int px = (int)lround(t->spec.size_pt * PT_TO_PX * k);
    if(px < 6) px = 6;
    const lv_font_t * font = ui_font_face_px(t->spec.font, px, false);
    bool chain = !ui_font_covers(font, t->label);
    if(chain) { font = ui_font_face_px(t->spec.font, px, true); *fell_back = true; }

    char text[256];
    snprintf(text, sizeof(text), "%s", t->label);
    int avail = (ly1 - ly0 + 1) - px * 3 / 4;             /* along the spine, less a margin */
    int across = lx1 - lx0 + 1;
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if(sz.x > avail && avail > 0) {
        /* shrink (widths scale with the size) down to the floor, then cut */
        int fit = (int)(px * (double)avail / sz.x);
        int floor_px = (int)(px * SHRINK_MIN);
        if(fit < floor_px) fit = floor_px;
        if(fit < 6) fit = 6;
        font = ui_font_face_px(t->spec.font, fit, chain);
        px = fit;
        lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if(sz.x > avail) trim_to(text, font, avail);
    }

    argb_t txt = { 0 };
    bool have_text = text[0] && render_text(text, font, t->spec.ink, &txt);

    if(sh->wide) {
        /* spine stands: text runs down it (a clockwise quarter turn) */
        argb_t rt = { 0 };
        if(have_text && rotate(&txt, true, &rt)) {
            int x = lx0 + (across - rt.w) / 2;
            int y;
            switch(t->spec.align) {
                case SHELF_ALIGN_START: y = ly0 + px / 2; break;
                case SHELF_ALIGN_END:   y = ly1 - px / 2 - rt.h; break;
                default:                y = ly0 + ((ly1 - ly0 + 1) - rt.h) / 2; break;
            }
            blend(&tile, &rt, x, y);
        }
        argb_free(&rt);
        *out = tile;
    }
    else {
        /* tape lies: the top of the spine goes left, text reads along it */
        argb_t lying = { 0 };
        if(!rotate(&tile, false, &lying)) { argb_free(&tile); argb_free(&txt); return false; }
        argb_free(&tile);
        /* the label box after the turn: x' = y, y' = (w-1) - x */
        int mx0 = ly0, mx1 = ly1, my0 = (w - 1) - lx1, my1 = (w - 1) - lx0;
        if(have_text) {
            int y = my0 + ((my1 - my0 + 1) - txt.h) / 2;
            int x;
            switch(t->spec.align) {
                case SHELF_ALIGN_START: x = mx0 + px / 2; break;
                case SHELF_ALIGN_END:   x = mx1 - px / 2 - txt.w; break;
                default:                x = mx0 + ((mx1 - mx0 + 1) - txt.w) / 2; break;
            }
            blend(&lying, &txt, x, y);
        }
        *out = lying;
    }
    argb_free(&txt);
    return true;
}

/* A flat tape in the back colour (a tile that couldn't be read). */
static void flat_master(const shelf_t * sh, argb_t * m)
{
    if(!argb_alloc(m, 180, MASTER_H)) return;
    uint32_t c = 0xFF000000u | sh->st.back;
    uint32_t edge = 0xFF000000u | ((sh->st.back >> 1) & 0x7F7F7F);
    for(int y = 0; y < m->h; y++)
        for(int x = 0; x < m->w; x++)
            m->px[(size_t)y * m->w + x] = (x < 6 || x >= m->w - 6 || y < 12 || y >= m->h - 12) ? edge : c;
}

static void tape_free(tape_t * t)
{
    argb_free(&t->rest);
    argb_free(&t->pull);
}

/* ---- the wood: the strip scaled to this screen, the rail's thickness
 * cut from its middle (the grain keeps the scale it was drawn at). ---- */
static bool load_wood(shelf_t * sh, const char * file, bool horizontal, argb_t * out)
{
    if(!file[0]) return false;
    char path[512];
    config_theme_asset(file, path, sizeof(path));
    argb_t strip;
    if(!load_png(path, &strip)) { fprintf(stderr, "simpleton-ui: shelf: no wood %s\n", path); return false; }
    argb_t scaled;
    int sw = (int)lround(strip.w * sh->S), shh = (int)lround(strip.h * sh->S);
    bool ok = scale_to(&strip, sw < 1 ? 1 : sw, shh < 1 ? 1 : shh, &scaled);
    argb_free(&strip);
    if(!ok) return false;
    /* crop the middle to the rail */
    int rw = horizontal ? scaled.w : (scaled.w < sh->rail ? scaled.w : sh->rail);
    int rh = horizontal ? (scaled.h < sh->rail ? scaled.h : sh->rail) : scaled.h;
    if(!argb_alloc(out, rw, rh)) { argb_free(&scaled); return false; }
    int ox = (scaled.w - rw) / 2, oy = (scaled.h - rh) / 2;
    for(int y = 0; y < rh; y++)
        memcpy(out->px + (size_t)y * rw, scaled.px + (size_t)(y + oy) * scaled.w + ox, (size_t)rw * 4);
    argb_free(&scaled);
    return true;
}

/* ------------------------------------------------------------ layout */

/* The ring is the visible items in order; the selected item's place in it
 * is the centre slot. */
static void rebuild_ring(shelf_t * sh)
{
    sh->nvis = 0;
    for(int i = 0; i < sh->count; i++) if(!sh->hidden[i]) sh->vis[sh->nvis++] = i;
    if(sh->nvis == 0 && sh->count) { sh->hidden[0] = false; sh->vis[sh->nvis++] = 0; }
}

static int ring_pos(const shelf_t * sh)
{
    for(int p = 0; p < sh->nvis; p++) if(sh->vis[p] == sh->sel) return p;
    return 0;
}

static int ring_index(const shelf_t * sh, int k)
{
    int n = sh->nvis;
    int p = (ring_pos(sh) + k) % n;
    if(p < 0) p += n;
    return sh->vis[p];
}

/* Put every slot, the rails, the wood offset and the pulled tape where
 * the current selection and slide say. */
static void layout(shelf_t * sh)
{
    int W = lv_obj_get_width(sh->obj), H = lv_obj_get_height(sh->obj);
    if(sh->count == 0 || sh->nvis == 0) return;

    if(sh->wide) {
        int cx = (W - sh->rest_w) / 2;                    /* the centre slot's left edge */
        int ty = sh->rail + ((H - 2 * sh->rail) - sh->rest_h) / 2;
        for(int k = -sh->K, s = 0; k <= sh->K; k++, s++) {
            lv_obj_t * img = sh->slots[s];
            const tape_t * t = &sh->tapes[ring_index(sh, k)];
            if(lv_image_get_src(img) != &t->rest_dsc) lv_image_set_src(img, &t->rest_dsc);
            lv_obj_set_pos(img, cx + k * sh->step + sh->offset, ty);
        }
        const tape_t * t = &sh->tapes[sh->sel];
        if(lv_image_get_src(sh->pulled) != &t->pull_dsc) lv_image_set_src(sh->pulled, &t->pull_dsc);
        lv_obj_set_pos(sh->pulled, (W - sh->pull_w) / 2 + sh->offset, (H - sh->pull_h) / 2);
        if(sh->rail_a) { lv_obj_set_pos(sh->rail_a, 0, 0); lv_image_set_offset_x(sh->rail_a, sh->wood_off + sh->offset); }
        if(sh->rail_b) { lv_obj_set_pos(sh->rail_b, 0, H - sh->rail); lv_image_set_offset_x(sh->rail_b, sh->wood_off + sh->offset); }
    }
    else {
        int cy = (H - sh->rest_w) / 2;                    /* rest_w = thickness = the slot pitch */
        int tx = sh->rail + ((W - 2 * sh->rail) - sh->rest_h) / 2;
        for(int k = -sh->K, s = 0; k <= sh->K; k++, s++) {
            lv_obj_t * img = sh->slots[s];
            const tape_t * t = &sh->tapes[ring_index(sh, k)];
            if(lv_image_get_src(img) != &t->rest_dsc) lv_image_set_src(img, &t->rest_dsc);
            lv_obj_set_pos(img, tx, cy + k * sh->step + sh->offset);
        }
        const tape_t * t = &sh->tapes[sh->sel];
        if(lv_image_get_src(sh->pulled) != &t->pull_dsc) lv_image_set_src(sh->pulled, &t->pull_dsc);
        lv_obj_set_pos(sh->pulled, (W - sh->pull_h) / 2, (H - sh->pull_w) / 2 + sh->offset);
        if(sh->rail_a) { lv_obj_set_pos(sh->rail_a, 0, 0); lv_image_set_offset_y(sh->rail_a, sh->wood_off + sh->offset); }
        if(sh->rail_b) { lv_obj_set_pos(sh->rail_b, W - sh->rail, 0); lv_image_set_offset_y(sh->rail_b, sh->wood_off + sh->offset); }
    }
}

static void slide_exec(void * var, int32_t v)
{
    shelf_t * sh = var;
    sh->offset = v;
    layout(sh);
}

static void pop_exec(void * var, int32_t v)
{
    shelf_t * sh = var;
    lv_image_set_scale(sh->pulled, (uint32_t)v);
}

/* ------------------------------------------------------------ public */

void shelf_style_defaults(shelf_style_t * st)
{
    memset(st, 0, sizeof(*st));
    st->rest = 0.889;
    st->pull = 0.96;
    st->rail = 60;
    st->back = 0x1A100C;
    st->shadow = 40;
}

bool shelf_is_wide(void)
{
    return ui_screen.w * 3 > ui_screen.h * 4;             /* wider than 4:3 */
}

static void shelf_delete_cb(lv_event_t * e)
{
    shelf_t * sh = lv_event_get_user_data(e);
    for(int i = 0; i < sh->count; i++) tape_free(&sh->tapes[i]);
    argb_free(&sh->wood_a);
    argb_free(&sh->wood_b);
    free(sh->slots);
    free(sh);
}

lv_obj_t * shelf_create(lv_obj_t * parent, const shelf_style_t * st)
{
    shelf_t * sh = calloc(1, sizeof(*sh));
    if(!sh) return NULL;
    sh->st = *st;
    sh->wide = shelf_is_wide();

    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(st->back), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_user_data(o, sh);
    lv_obj_add_event_cb(o, shelf_delete_cb, LV_EVENT_DELETE, sh);
    sh->obj = o;

    /* geometry */
    int side = sh->wide ? ui_screen.h : ui_screen.w;
    sh->S = (double)side / MASTER_H;
    sh->rest_h = (int)lround(MASTER_H * st->rest * sh->S);
    sh->rest_w = (int)lround(180 * st->rest * sh->S);
    sh->pull_h = (int)lround(MASTER_H * st->pull * sh->S);
    sh->pull_w = (int)lround(180 * st->pull * sh->S);
    sh->rail = (int)lround(st->rail * sh->S);
    sh->step = sh->rest_w;
    int span = sh->wide ? ui_screen.w : ui_screen.h;
    sh->K = (span / 2) / sh->step + 2;

    /* wood rails (optional) */
    if(load_wood(sh, sh->wide ? st->wood_h : st->wood_v, sh->wide, &sh->wood_a)) {
        dsc_wrap(&sh->wood_a_dsc, &sh->wood_a);
        if(load_wood(sh, sh->wide ? st->wood_h : st->wood_v, sh->wide, &sh->wood_b)) dsc_wrap(&sh->wood_b_dsc, &sh->wood_b);
        for(int i = 0; i < 2; i++) {
            lv_obj_t * r = lv_image_create(o);
            lv_image_set_src(r, i == 0 ? &sh->wood_a_dsc : (sh->wood_b.px ? &sh->wood_b_dsc : &sh->wood_a_dsc));
            lv_image_set_inner_align(r, LV_IMAGE_ALIGN_TILE);
            if(sh->wide) lv_obj_set_size(r, ui_screen.w, sh->rail);
            else         lv_obj_set_size(r, sh->rail, ui_screen.h);
            if(i == 0) sh->rail_a = r; else sh->rail_b = r;
        }
    }

    /* slots, then the pulled tape in front */
    sh->slots = calloc((size_t)(2 * sh->K + 1), sizeof(lv_obj_t *));
    for(int s = 0; s < 2 * sh->K + 1; s++) {
        lv_obj_t * img = lv_image_create(o);
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        sh->slots[s] = img;
    }
    sh->pulled = lv_image_create(o);
    lv_obj_add_flag(sh->pulled, LV_OBJ_FLAG_HIDDEN);
    if(st->shadow > 0) {
        int sw = (int)lround(st->shadow * sh->S);
        lv_obj_set_style_shadow_width(sh->pulled, sw, 0);
        lv_obj_set_style_shadow_opa(sh->pulled, LV_OPA_60, 0);
        lv_obj_set_style_shadow_color(sh->pulled, lv_color_black(), 0);
        lv_obj_set_style_shadow_offset_x(sh->pulled, sw / 4, 0);
        lv_obj_set_style_shadow_offset_y(sh->pulled, sw / 3, 0);
    }
    return o;
}

bool shelf_set_items(lv_obj_t * shelf, const shelf_item_t * items, int count, int selected)
{
    shelf_t * sh = lv_obj_get_user_data(shelf);
    if(!sh) return false;
    lv_anim_delete(sh, NULL);
    lv_anim_delete(sh->pulled, NULL);
    for(int i = 0; i < sh->count; i++) tape_free(&sh->tapes[i]);
    sh->count = 0;
    if(count > MAX_ITEMS) count = MAX_ITEMS;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int tiles_ok = 0, fallbacks = 0;
    for(int i = 0; i < count; i++) {
        tape_t * t = &sh->tapes[i];
        memset(t, 0, sizeof(*t));
        t->spec = items[i];
        apply_case(items[i].label ? items[i].label : "", items[i].letter_case, t->label, sizeof(t->label));

        argb_t master = { 0 };
        char path[512];
        config_theme_asset(items[i].tile, path, sizeof(path));
        if(items[i].tile[0] && load_png(path, &master)) tiles_ok++;
        else {
            if(items[i].tile[0]) fprintf(stderr, "simpleton-ui: shelf: cannot read tile %s\n", path);
            flat_master(sh, &master);
            if(!master.px) continue;
        }
        bool fb = false;
        bool ok = compose(sh, &master, t, sh->rest_h, &t->rest, &fb) && compose(sh, &master, t, sh->pull_h, &t->pull, &fb);
        argb_free(&master);
        if(!ok) { tape_free(t); continue; }
        if(fb) fallbacks++;
        dsc_wrap(&t->rest_dsc, &t->rest);
        dsc_wrap(&t->pull_dsc, &t->pull);
        sh->count++;
        if(i != sh->count - 1) { sh->tapes[sh->count - 1] = *t; memset(t, 0, sizeof(*t)); }   /* compact over a skipped one */
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "simpleton-ui: shelf: %s, %d tapes (%d tiles read, %d with fallback letters), %dx%d at rest, %dx%d pulled, %ld ms\n",
            sh->wide ? "bookshelf" : "stack", sh->count, tiles_ok, fallbacks,
            sh->rest_w, sh->rest_h, sh->pull_w, sh->pull_h,
            (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    if(sh->count == 0) return false;
    memset(sh->hidden, 0, sizeof(sh->hidden));
    rebuild_ring(sh);

    for(int s = 0; s < 2 * sh->K + 1; s++) lv_obj_remove_flag(sh->slots[s], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sh->pulled, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_scale(sh->pulled, 256);
    sh->sel = (selected >= 0 && selected < sh->count) ? selected : 0;
    sh->offset = 0;
    layout(sh);
    return tiles_ok > 0;
}

void shelf_select(lv_obj_t * shelf, int index)
{
    shelf_t * sh = lv_obj_get_user_data(shelf);
    if(!sh || sh->count == 0) return;
    lv_anim_delete(sh, NULL);
    lv_anim_delete(sh->pulled, NULL);
    index = ((index % sh->count) + sh->count) % sh->count;
    if(sh->hidden[index]) index = sh->vis[0];
    sh->sel = index;
    sh->offset = 0;
    lv_image_set_scale(sh->pulled, 256);
    layout(sh);
}

void shelf_set_visible(lv_obj_t * shelf, int index, bool visible)
{
    shelf_t * sh = lv_obj_get_user_data(shelf);
    if(!sh || index < 0 || index >= sh->count) return;
    if(sh->hidden[index] == !visible) return;
    sh->hidden[index] = !visible;
    int next = sh->sel;
    if(!visible && index == sh->sel) {
        /* hand the selection to the next visible item after it */
        next = ring_index(sh, 1);
        if(next == index) next = -1;
    }
    rebuild_ring(sh);
    if(next < 0) next = sh->vis[0];
    lv_anim_delete(sh, NULL);
    lv_anim_delete(sh->pulled, NULL);
    sh->sel = next;
    sh->offset = 0;
    lv_image_set_scale(sh->pulled, 256);
    layout(sh);
}

void shelf_move(lv_obj_t * shelf, int dir)
{
    shelf_t * sh = lv_obj_get_user_data(shelf);
    if(!sh || sh->nvis < 2 || dir == 0) return;
    dir = dir > 0 ? 1 : -1;

    /* settle any slide still running, then start from the new neighbour */
    lv_anim_delete(sh, NULL);
    lv_anim_delete(sh->pulled, NULL);
    sh->sel = ring_index(sh, dir);
    sh->offset = dir * sh->step;             /* the ring is still where it was... */
    sh->wood_off -= dir * sh->step;          /* ...and the grain keeps travelling */
    layout(sh);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sh);
    lv_anim_set_exec_cb(&a, slide_exec);
    lv_anim_set_values(&a, sh->offset, 0);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* the new tape pops out from the resting size */
    int from = (int)lround(256.0 * sh->st.rest / sh->st.pull);
    lv_anim_init(&a);
    lv_anim_set_var(&a, sh);
    lv_anim_set_exec_cb(&a, pop_exec);
    lv_anim_set_values(&a, from, 256);
    lv_anim_set_duration(&a, POP_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

int shelf_selected(const lv_obj_t * shelf)
{
    const shelf_t * sh = lv_obj_get_user_data((lv_obj_t *)shelf);
    return sh ? sh->sel : 0;
}

int shelf_count(const lv_obj_t * shelf)
{
    const shelf_t * sh = lv_obj_get_user_data((lv_obj_t *)shelf);
    return sh ? sh->count : 0;
}

bool shelf_handle_action(lv_obj_t * shelf, ui_action_t a)
{
    shelf_t * sh = lv_obj_get_user_data(shelf);
    if(!sh) return false;
    if(sh->wide) {
        if(a == ACT_LEFT)  { shelf_move(shelf, -1); return true; }
        if(a == ACT_RIGHT) { shelf_move(shelf, +1); return true; }
    }
    else {
        if(a == ACT_UP)   { shelf_move(shelf, -1); return true; }
        if(a == ACT_DOWN) { shelf_move(shelf, +1); return true; }
    }
    return false;
}
