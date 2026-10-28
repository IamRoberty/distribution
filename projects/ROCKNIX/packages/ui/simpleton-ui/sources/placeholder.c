/*
 * SimpletonOS UI - designed placeholder art (implementation). See placeholder.h.
 *
 * 0.9: label text falls back to Noto Sans for any letter the theme's font
 * lacks (accents, Cyrillic, Greek), scaled to the same cap height, so a
 * Russian album still gets a readable cassette label.
 */
#define _GNU_SOURCE
#include "placeholder.h"
#include "config.h"
#include "strings.h"

#include <ctype.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"          /* public domain, github.com/nothings/stb */
#pragma GCC diagnostic pop


#define MAX_ZONES  8
#define MAX_LINES  4
#define MAX_WAYS   32
#define MAX_FONTS  24
#define MAX_DESIGNS 4
#define MAX_AREAS  12

/* A text area (11 Oct, Ian's record): stacked rectangles of different
 * widths that together make one shape - e.g. a narrow strip above a wide
 * one on the upper half of a round label. The item's text is fitted into
 * the whole shape at the largest size that fits: a short word big, a
 * longer name over two or three lines, or one line in the wide part. */
#define AREA_LINES     3
#define AREA_CAP_FLOOR 0.018f    /* of the box: 19 px at 1080, 13 at 720 */
#define AREA_STEP      0.96f     /* size ladder */

#define SHRINK_MIN 0.55f     /* a long line shrinks to 55 % before it is trimmed */

/* ------------------------------------------------------------- types */

typedef struct { uint8_t * px; int w, h; } gray_t;

typedef struct { char name[32]; gray_t mask; } zone_t;

typedef struct { float cx, baseline, cap, room; } line_t;     /* output px */

typedef struct { int item; float x0, y0, x1, y1; } area_t;    /* output px; item 0 artist, 1 album, 2 title */

typedef struct {
    char           file[64];
    uint8_t *      data;
    stbtt_fontinfo info;
    int            cap_units;    /* height of 'H' in font units */
} font_t;

typedef struct {
    uint32_t zone_rgb[MAX_ZONES];
    uint32_t ink;
    char     name[48];
} colourway_t;

/* A design: its base picture, zone masks and text lines (0.17h: several
 * may be loaded - the theme's cassette and, say, a record - and a design
 * is asked for by name). */
typedef struct {
    char   name[32];
    gray_t base;
    gray_t light;              /* optional light.png: white adds light after the colours (0.17i), px NULL = none */
    zone_t zones[MAX_ZONES];
    int    zone_count;
    line_t lines[MAX_LINES];
    int    line_count;
    area_t areas[MAX_AREAS];
    int    area_count;
    bool   loaded, failed;
} design_t;

/* ------------------------------------------------------------- state */

static char  share_dir[256];
static int   box;

static design_t designs[MAX_DESIGNS];
static int      design_count;
static design_t * main_design;         /* the theme's "design" line */

static font_t fonts[MAX_FONTS];
static int    font_count;
static int    fallback_font = -1;   /* Noto Sans: letters the theme's fonts lack (0.9) */
static int    line_font[MAX_LINES];
static bool   line_caps[MAX_LINES];

static colourway_t ways[MAX_WAYS];
static int         way_count;
static int         last_way = -1;
static char        theme_name[32];

static bool ready;

/* The album page's colours for one album (the cassette a track of it gets
 * in now-playing matches its tile): read by the art worker thread. */
static pthread_mutex_t prefer_lock = PTHREAD_MUTEX_INITIALIZER;
static char     prefer_artist[256], prefer_album[256];
static char **  prefer_uris;               /* the album's tracks: a track is known by its path first (tags may differ) */
static int      prefer_nuris;
static uint32_t prefer_rgb[MAX_ZONES];     /* by zone index of the main design */
static bool     prefer_set[MAX_ZONES];
static uint32_t prefer_ink;
static char     prefer_font[64];           /* the album line's face, "" = the theme's */
static bool     prefer_caps, have_prefer;
static placeholder_tint_t prefer_tints[8];   /* the page's tints by name, for the record too (0.23b) */
static int      prefer_ntints;
static uint32_t prefer_fallback;

/* per-track colours (0.23e): the view's own, for each track of the queue */
#define TRACK_PREFS 512
typedef struct { char uri[400]; placeholder_tint_t tints[8]; char zones[8][16]; int ntints; uint32_t fallback, ink; char font[64]; bool caps; } track_pref_t;
static track_pref_t * track_prefs;
static int track_pref_n;

void placeholder_prefer_tracks_begin(void)
{
    pthread_mutex_lock(&prefer_lock);
    track_pref_n = 0;
    pthread_mutex_unlock(&prefer_lock);
}

void placeholder_prefer_track(const char * uri, const placeholder_tint_t * tints, int ntints, uint32_t fallback,
                              uint32_t ink, const char * album_font, bool album_caps)
{
    if(!uri || !uri[0]) return;
    pthread_mutex_lock(&prefer_lock);
    if(!track_prefs) track_prefs = calloc(TRACK_PREFS, sizeof(*track_prefs));
    if(track_prefs && track_pref_n < TRACK_PREFS) {
        track_pref_t * t = &track_prefs[track_pref_n++];
        snprintf(t->uri, sizeof(t->uri), "%.399s", uri);
        t->ntints = ntints < 8 ? ntints : 8;
        for(int i = 0; i < t->ntints; i++) { t->tints[i].rgb = tints[i].rgb; snprintf(t->zones[i], sizeof(t->zones[i]), "%s", tints[i].zone ? tints[i].zone : ""); t->tints[i].zone = t->zones[i]; }
        t->fallback = fallback;
        t->ink = ink;
        snprintf(t->font, sizeof(t->font), "%s", album_font ? album_font : "");
        t->caps = album_caps;
    }
    pthread_mutex_unlock(&prefer_lock);
}

/* -------------------------------------------------------------- files */

static uint8_t * read_file(const char * path, size_t * len)
{
    *len = 0;
    FILE * f = fopen(path, "rb");
    if(!f) return NULL;
    struct stat st;
    if(fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > (64 << 20)) { fclose(f); return NULL; }
    uint8_t * buf = malloc((size_t)st.st_size + 1);
    if(buf && fread(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size) { *len = (size_t)st.st_size; buf[*len] = 0; }
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

typedef struct { const uint8_t * p; size_t len, off; } png_mem_t;
static void png_mem_read(png_structp png, png_bytep dst, png_size_t n)
{
    png_mem_t * m = png_get_io_ptr(png);
    if(m->off + n > m->len) png_error(png, "eof");
    memcpy(dst, m->p + m->off, n);
    m->off += n;
}

/* Decode any PNG to 8-bit grayscale. */
static bool load_gray(const char * path, gray_t * out)
{
    size_t len;
    uint8_t * data = read_file(path, &len);
    if(!data) return false;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if(!png) { free(data); return false; }
    png_infop info = png_create_info_struct(png);
    if(!info) { png_destroy_read_struct(&png, NULL, NULL); free(data); return false; }
    uint8_t * px = NULL;
    png_bytep * rows = NULL;
    if(setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        free(px); free(rows); free(data);
        return false;
    }
    png_mem_t m = { data, len, 0 };
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int color = png_get_color_type(png, info), depth = png_get_bit_depth(png, info);
    if(w == 0 || h == 0 || w > 4096 || h > 4096) longjmp(png_jmpbuf(png), 1);

    if(color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if(color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if(png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if(depth == 16) png_set_strip_16(png);
    if(color & PNG_COLOR_MASK_COLOR) png_set_rgb_to_gray_fixed(png, 1, 21268, 71510);
    png_set_strip_alpha(png);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if(png_get_channels(png, info) != 1) longjmp(png_jmpbuf(png), 1);

    px = malloc((size_t)w * h);
    rows = malloc(sizeof(png_bytep) * h);
    if(!px || !rows) longjmp(png_jmpbuf(png), 1);
    for(png_uint_32 y = 0; y < h; y++) rows[y] = px + (size_t)y * w;
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    free(rows);
    free(data);
    out->px = px;
    out->w = (int)w;
    out->h = (int)h;
    return true;
}

/* Box-filter a grayscale image down to dw x dh (used for the 1080 master
 * -> 720 panel; the master is never enlarged in practice, but bilinear
 * covers a bigger screen than the master). */
static bool resample_gray(gray_t * g, int dw, int dh)
{
    if(g->w == dw && g->h == dh) return true;
    uint8_t * dst = malloc((size_t)dw * dh);
    if(!dst) return false;
    int sw = g->w, sh = g->h;
    if(sw >= dw && sh >= dh) {
        for(int y = 0; y < dh; y++) {
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if(y1 <= y0) y1 = y0 + 1;
            for(int x = 0; x < dw; x++) {
                int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
                if(x1 <= x0) x1 = x0 + 1;
                unsigned sum = 0, n = 0;
                for(int yy = y0; yy < y1; yy++)
                    for(int xx = x0; xx < x1; xx++) { sum += g->px[(size_t)yy * sw + xx]; n++; }
                dst[(size_t)y * dw + x] = (uint8_t)(sum / n);
            }
        }
    }
    else {
        for(int y = 0; y < dh; y++) {
            double fy = (y + 0.5) * sh / dh - 0.5;
            int y0 = (int)floor(fy); double ty = fy - y0;
            if(y0 < 0) { y0 = 0; ty = 0; }
            int y1 = y0 + 1 < sh ? y0 + 1 : y0;
            for(int x = 0; x < dw; x++) {
                double fx = (x + 0.5) * sw / dw - 0.5;
                int x0 = (int)floor(fx); double tx = fx - x0;
                if(x0 < 0) { x0 = 0; tx = 0; }
                int x1 = x0 + 1 < sw ? x0 + 1 : x0;
                double top = g->px[(size_t)y0 * sw + x0] + (g->px[(size_t)y0 * sw + x1] - g->px[(size_t)y0 * sw + x0]) * tx;
                double bot = g->px[(size_t)y1 * sw + x0] + (g->px[(size_t)y1 * sw + x1] - g->px[(size_t)y1 * sw + x0]) * tx;
                dst[(size_t)y * dw + x] = (uint8_t)lround(top + (bot - top) * ty);
            }
        }
    }
    free(g->px);
    g->px = dst;
    g->w = dw;
    g->h = dh;
    return true;
}

/* ------------------------------------------------------------- parsing */

static bool parse_hex(const char * s, uint32_t * out)
{
    if(*s == '#') s++;
    if(strlen(s) < 6) return false;
    char * end;
    unsigned long v = strtoul(s, &end, 16);
    if(end - s != 6) return false;
    *out = (uint32_t)v;
    return true;
}

static int zone_index(const design_t * d, const char * name)
{
    for(int i = 0; i < d->zone_count; i++) if(strcmp(d->zones[i].name, name) == 0) return i;
    return -1;
}

/* Fonts and designs load on first use, from either thread (the art worker
 * renders now-playing's cassette, the UI thread the album page's): one lock
 * round the loading; a loaded entry never changes. */
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;

static int font_index(const char * file)
{
    pthread_mutex_lock(&load_lock);
    for(int i = 0; i < font_count; i++) if(strcmp(fonts[i].file, file) == 0) { pthread_mutex_unlock(&load_lock); return i; }
    if(font_count == MAX_FONTS) { pthread_mutex_unlock(&load_lock); return -1; }
    font_t * f = &fonts[font_count];
    char path[512];
    snprintf(path, sizeof(path), "%s/fonts/%s", share_dir, file);
    size_t len;
    f->data = read_file(path, &len);
    if(!f->data || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        fprintf(stderr, "simpleton-ui: placeholder: cannot load font %s\n", path);
        free(f->data); f->data = NULL;
        pthread_mutex_unlock(&load_lock);
        return -1;
    }
    int x0, y0, x1, y1;
    f->cap_units = stbtt_GetCodepointBox(&f->info, 'H', &x0, &y0, &x1, &y1) ? y1 : 700;
    snprintf(f->file, sizeof(f->file), "%s", file);
    int i = font_count++;
    pthread_mutex_unlock(&load_lock);
    return i;
}

/* layout.txt: "size W H" then "text cx baseline cap room" per line. */
static bool load_layout(design_t * d)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/placeholder/%s/layout.txt", share_dir, d->name);
    FILE * f = fopen(path, "r");
    if(!f) return false;
    float mw = 0, mh = 0;
    char line[256];
    d->line_count = 0;
    d->area_count = 0;
    while(fgets(line, sizeof(line), f)) {
        float a, b, c, e;
        char what[16];
        if(sscanf(line, "size %f %f", &a, &b) == 2) { mw = a; mh = b; }
        else if(sscanf(line, "text %f %f %f %f", &a, &b, &c, &e) == 4 && d->line_count < MAX_LINES && mw > 0) {
            float sc = (float)box / mw;
            d->lines[d->line_count++] = (line_t){ a * sc, b * sc, c * sc, e * sc };
        }
        else if(sscanf(line, "area %15s %f %f %f %f", what, &a, &b, &c, &e) == 5 && d->area_count < MAX_AREAS && mw > 0
                && c > a && e > b) {
            int item = strcmp(what, "artist") == 0 ? 0 : strcmp(what, "album") == 0 ? 1 : strcmp(what, "title") == 0 ? 2 : -1;
            if(item < 0) { fprintf(stderr, "simpleton-ui: placeholder: %s: unknown area \"%s\" (artist, album or title)\n", path, what); continue; }
            float sc = (float)box / mw;
            d->areas[d->area_count++] = (area_t){ item, a * sc, b * sc, c * sc, e * sc };
        }
    }
    fclose(f);
    (void)mh;
    return mw > 0;                      /* a design with no text lines is fine (a record with nothing written on it) */
}

static bool load_design_into(design_t * d, const char * design)
{
    char path[512];
    snprintf(d->name, sizeof(d->name), "%s", design);
    snprintf(path, sizeof(path), "%s/placeholder/%s/base.png", share_dir, design);
    if(!load_gray(path, &d->base) || !resample_gray(&d->base, box, box)) return false;

    static const char * const zone_names[] = { "background", "shell", "label", "disc", "sleeve", "centre", "groove", "spindle", NULL };
    d->zone_count = 0;
    for(int i = 0; zone_names[i] && d->zone_count < MAX_ZONES; i++) {
        snprintf(path, sizeof(path), "%s/placeholder/%s/mask-%s.png", share_dir, design, zone_names[i]);
        zone_t * z = &d->zones[d->zone_count];
        if(!load_gray(path, &z->mask)) continue;
        if(!resample_gray(&z->mask, box, box)) return false;
        snprintf(z->name, sizeof(z->name), "%s", zone_names[i]);
        d->zone_count++;
    }
    /* light.png is optional: a sheen on a dark disc can't come from the base,
     * since the colours multiply onto it and nothing gets lighter than its zone */
    snprintf(path, sizeof(path), "%s/placeholder/%s/light.png", share_dir, design);
    if(load_gray(path, &d->light) && !resample_gray(&d->light, box, box)) return false;
    return d->zone_count > 0 && load_layout(d);
}

/* A design by name, loaded on first ask (NULL when its files aren't there). */
static design_t * design_get(const char * name)
{
    if(!name || !name[0]) return main_design;
    pthread_mutex_lock(&load_lock);
    design_t * d = NULL;
    for(int i = 0; i < design_count && !d; i++) if(strcmp(designs[i].name, name) == 0) d = &designs[i];
    if(!d && design_count < MAX_DESIGNS) {
        d = &designs[design_count++];
        memset(d, 0, sizeof(*d));
        d->loaded = load_design_into(d, name);
        d->failed = !d->loaded;
        if(!d->loaded) fprintf(stderr, "simpleton-ui: placeholder: no usable design \"%s\" under %s/placeholder\n", name, share_dir);
        else fprintf(stderr, "simpleton-ui: placeholder: design %s, %d zones, %d text lines\n", d->name, d->zone_count, d->line_count);
    }
    pthread_mutex_unlock(&load_lock);
    return d && d->loaded ? d : NULL;
}

/* <name>.theme:
 *   design cassette
 *   font <line> <file.ttf> caps|mixed
 *   colourway zone=RRGGBB ... text=RRGGBB ; human name
 */
static bool load_theme(const char * name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/themes/%s.theme", share_dir, name);
    FILE * f = fopen(path, "r");
    if(!f) { fprintf(stderr, "simpleton-ui: placeholder: no theme %s\n", path); return false; }

    char design[64] = "";
    char line[512];
    way_count = 0;
    for(int i = 0; i < MAX_LINES; i++) { line_font[i] = -1; line_caps[i] = false; }

    /* pass 1: design + fonts (fonts need no design; colourways need zones) */
    while(fgets(line, sizeof(line), f)) {
        char a[64], b[64];
        int n;
        if(sscanf(line, "design %63s", a) == 1) snprintf(design, sizeof(design), "%s", a);
        else if(sscanf(line, "font %d %63s %63s", &n, a, b) == 3 && n >= 1 && n <= MAX_LINES) {
            line_font[n - 1] = font_index(a);
            line_caps[n - 1] = strcmp(b, "caps") == 0;
        }
    }
    if(!design[0] || !(main_design = design_get(design))) { fclose(f); return false; }

    rewind(f);
    while(fgets(line, sizeof(line), f) && way_count < MAX_WAYS) {
        if(strncmp(line, "colourway", 9) != 0) continue;
        colourway_t * w = &ways[way_count];
        memset(w, 0, sizeof(*w));
        w->ink = 0x000000;
        bool ok = true;
        char * semi = strchr(line, ';');
        if(semi) { *semi = 0; snprintf(w->name, sizeof(w->name), "%s", semi + 1); w->name[strcspn(w->name, "\n")] = 0;
                   char * s = w->name; while(*s == ' ') s++; memmove(w->name, s, strlen(s) + 1); }
        char * save;
        for(char * tok = strtok_r(line + 9, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
            char * eq = strchr(tok, '=');
            if(!eq) continue;
            *eq = 0;
            uint32_t rgb;
            if(!parse_hex(eq + 1, &rgb)) { ok = false; break; }
            if(strcmp(tok, "text") == 0) w->ink = rgb;
            else {
                int z = zone_index(main_design, tok);
                if(z >= 0) w->zone_rgb[z] = rgb;
            }
        }
        if(ok) way_count++;
    }
    fclose(f);
    snprintf(theme_name, sizeof(theme_name), "%s", name);
    return way_count > 0;
}

/* --------------------------------------------------------------- text */

static uint32_t utf8_next(const char ** s)
{
    const uint8_t * p = (const uint8_t *)*s;
    uint32_t c = p[0];
    int n = 0;
    if(c >= 0xF0) { c &= 0x07; n = 3; }
    else if(c >= 0xE0) { c &= 0x0F; n = 2; }
    else if(c >= 0xC0) { c &= 0x1F; n = 1; }
    (*s)++;
    for(int i = 0; i < n && ((*s)[0] & 0xC0) == 0x80; i++, (*s)++) c = (c << 6) | ((*s)[0] & 0x3F);
    return c;
}

/* Which font draws code point `c`: the line's own font, else the fallback
 * (scaled so its capitals match the line's cap height). Returns the glyph
 * index, 0 when neither has it. */
static int pick_glyph(const font_t * f, uint32_t c, float scale, const font_t ** use, float * use_scale)
{
    int g = stbtt_FindGlyphIndex(&f->info, (int)c);
    *use = f;
    *use_scale = scale;
    if(!g && fallback_font >= 0 && &fonts[fallback_font] != f) {
        const font_t * fb = &fonts[fallback_font];
        g = stbtt_FindGlyphIndex(&fb->info, (int)c);
        if(g) { *use = fb; *use_scale = scale * f->cap_units / fb->cap_units; }
    }
    return g;
}

/* Width of `text` at `scale`, skipping glyphs no font has. */
static float text_width(const font_t * f, const char * text, float scale)
{
    float w = 0;
    int prev = 0;
    const font_t * prev_font = NULL;
    for(const char * s = text; *s;) {
        uint32_t c = utf8_next(&s);
        const font_t * use; float sc;
        int g = pick_glyph(f, c, scale, &use, &sc);
        if(!g) continue;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&use->info, g, &adv, &lsb);
        if(prev && prev_font == use) adv += stbtt_GetGlyphKernAdvance(&use->info, prev, g);
        w += adv * sc;
        prev = g;
        prev_font = use;
    }
    return w;
}

static void draw_text(uint32_t * out, const font_t * f, const char * text, float scale, float x, float y, uint32_t ink)
{
    float ir = (ink >> 16) & 0xFF, ig = (ink >> 8) & 0xFF, ib = ink & 0xFF;
    int prev = 0;
    const font_t * prev_font = NULL;
    for(const char * s = text; *s;) {
        uint32_t c = utf8_next(&s);
        const font_t * use; float sc;
        int g = pick_glyph(f, c, scale, &use, &sc);
        if(!g) continue;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&use->info, g, &adv, &lsb);
        if(prev && prev_font == use) x += stbtt_GetGlyphKernAdvance(&use->info, prev, g) * sc;
        prev = g;
        prev_font = use;

        int ix = (int)floorf(x);
        float shift = x - ix;
        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&use->info, g, sc, sc, shift, 0, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if(gw > 0 && gh > 0) {
            uint8_t * cov = malloc((size_t)gw * gh);
            if(cov) {
                stbtt_MakeGlyphBitmapSubpixel(&use->info, cov, gw, gh, gw, sc, sc, shift, 0, g);
                int by = (int)lroundf(y);
                for(int yy = 0; yy < gh; yy++) {
                    int py = by + y0 + yy;
                    if(py < 0 || py >= box) continue;
                    for(int xx = 0; xx < gw; xx++) {
                        int px = ix + x0 + xx;
                        int a = cov[(size_t)yy * gw + xx];
                        if(px < 0 || px >= box || !a) continue;
                        uint32_t * d = &out[(size_t)py * box + px];
                        float r = (*d >> 16) & 0xFF, gg = (*d >> 8) & 0xFF, b = *d & 0xFF, t = a / 255.0f;
                        r += (ir - r) * t; gg += (ig - gg) * t; b += (ib - b) * t;
                        *d = 0xFF000000u | ((uint32_t)lroundf(r) << 16) | ((uint32_t)lroundf(gg) << 8) | (uint32_t)lroundf(b);
                    }
                }
                free(cov);
            }
        }
        x += adv * sc;
    }
}

static void upper_ascii(char * s)
{
    str_upper_utf8(s);          /* the name stays; accented, Greek, Cyrillic too (11 Oct) */
}

/* Centre `text` on the line: shrink to SHRINK_MIN if too wide, then trim
 * with an ellipsis. */
static void render_line_min(uint32_t * out, const line_t * ln, int fi, bool caps, const char * text, uint32_t ink, float shrink_min);

static void render_line(uint32_t * out, const line_t * ln, int fi, bool caps, const char * text, uint32_t ink)
{
    render_line_min(out, ln, fi, caps, text, ink, SHRINK_MIN);
}

/* ... shrinking no further than `shrink_min` (1 = trim at the given size) */
/* A title's tail, as textfit.c's split_bracket has it (the cache links no
 * LVGL, so the rule lives here too): after the first "(", "[" (kept with the
 * tail), ":", ";" (kept with the head) or a spaced dash (dropped); not a
 * comma - too many titles have one (0.41a, Ian). */
static bool tail_split(const char * title, char * head, size_t hl, char * tail, size_t tl)
{
    const char * q = NULL, * t = NULL; size_t hn = 0;
    bool letters = false;                                        /* a head with no letters yet is no head: scan on (0.41b) */
    for(const char * p = title; *p && !q; p++) {
        if(isalpha((unsigned char)*p) || (unsigned char)*p >= 0x80) { letters = true; continue; }
        if(!letters) continue;
        if(*p == '(' || *p == '[') { q = p; hn = (size_t)(p - title); t = p; }
        else if((*p == ':' || *p == ';') && (p[1] == ' ' || p[1] == 0)) { q = p; hn = (size_t)(p - title) + 1; t = p + 1; }
        else if(*p == ' ' && p[1] == '-' && p[2] == ' ') { q = p; hn = (size_t)(p - title); t = p + 3; }
        else if(*p == ' ' && memcmp(p + 1, "\xE2\x80\x93", 3) == 0 && p[4] == ' ') { q = p; hn = (size_t)(p - title); t = p + 5; }
    }
    if(!q || q == title) return false;
    while(t && *t == ' ') t++;
    if(!t || !*t) return false;
    while(hn > 0 && title[hn - 1] == ' ') hn--;
    if(hn == 0 || hn >= hl) return false;
    memcpy(head, title, hn); head[hn] = 0;
    snprintf(tail, tl, "%s", t);
    return tail[0] != 0;
}

static void render_line_min(uint32_t * out, const line_t * ln, int fi, bool caps, const char * text, uint32_t ink, float shrink_min)
{
    if(fi < 0 || !text || !text[0]) return;
    const font_t * f = &fonts[fi];
    char buf[512], tail[256];
    bool has_tail = tail_split(text, buf, sizeof(buf), tail, sizeof(tail));
    if(!has_tail) snprintf(buf, sizeof(buf), "%s", text);
    if(caps) { upper_ascii(buf); if(has_tail) upper_ascii(tail); }

    float scale = ln->cap / f->cap_units;
    if(has_tail) {
        /* the tail at 70 % after the head, on the baseline (0.41): both
         * shrink together to the room; a tail that would take half the room
         * is dropped rather than squeezing the head */
        float gap = ln->cap * 0.35f;
        float wh = text_width(f, buf, scale), wt = text_width(f, tail, scale * 0.7f);
        if(wh + gap + wt > ln->room) {
            float s2 = scale * ln->room / (wh + gap + wt);
            if(s2 >= scale * shrink_min && wt * s2 / scale < ln->room / 2) { scale = s2; gap = ln->cap * 0.35f * s2 / (ln->cap / f->cap_units); wh = text_width(f, buf, scale); wt = text_width(f, tail, scale * 0.7f); }
            else has_tail = false;                                /* the head alone, as any line */
        }
        if(has_tail) {
            float x = ln->cx - (wh + gap + wt) / 2;
            draw_text(out, f, buf, scale, x, ln->baseline, ink);
            draw_text(out, f, tail, scale * 0.7f, x + wh + gap, ln->baseline, ink);
            return;
        }
    }
    float w = text_width(f, buf, scale);
    if(w > ln->room) {
        float s2 = scale * ln->room / w;
        scale = s2 > scale * shrink_min ? s2 : scale * shrink_min;
        w = text_width(f, buf, scale);
    }
    if(w > ln->room + 0.5f) {
        const char * ell = stbtt_FindGlyphIndex(&f->info, 0x2026) ? "\xE2\x80\xA6" : "...";
        size_t n = strlen(buf);
        for(;;) {
            while(n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;   /* back to a char boundary */
            if(n > 0) n--;
            while(n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
            char t[sizeof(buf)];
            size_t k = n;
            while(k > 0 && buf[k - 1] == ' ') k--;
            snprintf(t, sizeof(t), "%.*s%s", (int)k, buf, ell);
            w = text_width(f, t, scale);
            if(w <= ln->room + 0.5f || n == 0) { snprintf(buf, sizeof(buf), "%s", t); break; }
        }
    }
    draw_text(out, f, buf, scale, ln->cx - w / 2, ln->baseline, ink);
}

/* --------------------------------------------------------------- tint */

/* out = base * (sum_k mask_k * colour_k / max(sum_k mask_k, 1) + (1 - min(sum, 1))) */
static void tint(uint32_t * out, const design_t * d, const uint32_t * zone_rgb)
{
    size_t n = (size_t)box * box;
    for(size_t i = 0; i < n; i++) {
        unsigned sum = 0, r = 0, g = 0, b = 0;
        for(int z = 0; z < d->zone_count; z++) {
            unsigned m = d->zones[z].mask.px[i];
            if(!m) continue;
            sum += m;
            r += m * ((zone_rgb[z] >> 16) & 0xFF);
            g += m * ((zone_rgb[z] >> 8) & 0xFF);
            b += m * (zone_rgb[z] & 0xFF);
        }
        unsigned tot = sum > 255 ? sum : 255;             /* /255 keeps colour true where sum == 255 */
        unsigned gap = sum >= 255 ? 0 : 255 - sum;        /* uncovered part stays untinted */
        unsigned mr = r / tot + gap, mg = g / tot + gap, mb = b / tot + gap;   /* 0..255 multipliers */
        if(mr > 255) mr = 255;
        if(mg > 255) mg = 255;
        if(mb > 255) mb = 255;
        unsigned v = d->base.px[i];
        unsigned cr = v * mr / 255, cg = v * mg / 255, cb = v * mb / 255;
        if(d->light.px) {                                   /* screen with white x light: out + (255 - out) * l */
            unsigned l = d->light.px[i];
            cr += (255 - cr) * l / 255;
            cg += (255 - cg) * l / 255;
            cb += (255 - cb) * l / 255;
        }
        out[i] = 0xFF000000u | (cr << 16) | (cg << 8) | cb;
    }
}

/* -------------------------------------------------------------- public */

bool placeholder_init(int box_size)
{
    box = box_size;
    snprintf(share_dir, sizeof(share_dir), "%s", config_share_dir());
    const char * name = config_theme_name();
    ready = load_theme(name) || (strcmp(name, THEME_DEFAULT) != 0 && load_theme(THEME_DEFAULT));
    fallback_font = font_index("NotoSans-Medium.ttf");   /* label letters the theme's fonts lack */
    if(ready) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        fprintf(stderr, "simpleton-ui: placeholder: theme %s, design %s, %d colourways, %d zones, %d text lines, box %d\n",
                theme_name, main_design->name, way_count, main_design->zone_count, main_design->line_count, box);
    }
    else fprintf(stderr, "simpleton-ui: placeholder: assets not usable under %s, using plain placeholder\n", share_dir);
    return ready;
}

int placeholder_colourway_count(void) { return ready ? way_count : 0; }

bool placeholder_colourway(int i, uint32_t * label_rgb, uint32_t * ink_rgb)
{
    if(!ready || i < 0 || i >= way_count) return false;
    const colourway_t * w = &ways[i];
    int z = zone_index(main_design, "label");
    if(label_rgb) *label_rgb = z >= 0 ? w->zone_rgb[z] : w->zone_rgb[0];
    if(ink_rgb) *ink_rgb = w->ink;
    return true;
}

bool placeholder_has_design(const char * name)
{
    if(!ready) return false;
    return design_get(name) != NULL;
}

const char * placeholder_main_design(void) { return ready ? main_design->name : ""; }

int placeholder_design_lines(const char * name)
{
    if(!ready) return 0;
    const design_t * d = design_get(name);
    return d ? d->line_count : 0;
}

/* Zone colours for `d` from name/colour pairs; zones not named take `fallback`. */
static void tints_to_zones(const design_t * d, const placeholder_tint_t * tints, int ntints, uint32_t fallback, uint32_t * zone_rgb)
{
    for(int z = 0; z < d->zone_count; z++) {
        zone_rgb[z] = fallback;
        for(int i = 0; i < ntints; i++) if(tints[i].zone && strcmp(tints[i].zone, d->zones[z].name) == 0) { zone_rgb[z] = tints[i].rgb; break; }
    }
}

/* The width a line may have between cap top `t` and baseline `b` in an
 * item's shape: the narrowest of its rectangles the line crosses, centred
 * on that one; 0 when part of the line would be outside the shape. */
static float area_width(const design_t * d, int item, float t, float b, float * cx)
{
    float w = 0, lo = 1e9f, hi = -1e9f;
    for(int i = 0; i < d->area_count; i++) {
        const area_t * a = &d->areas[i];
        if(a->item != item) continue;
        if(a->y0 < lo) lo = a->y0;
        if(a->y1 > hi) hi = a->y1;
        if(a->y1 <= t || a->y0 >= b) continue;
        float aw = a->x1 - a->x0;
        if(w == 0 || aw < w) { w = aw; *cx = (a->x0 + a->x1) / 2; }
    }
    return t >= lo - 0.5f && b <= hi + 0.5f ? w : 0;
}

/* One fitting of an item's text into its shape: the words, and for the
 * size being tried each line's baseline, room and centre. */
typedef struct {
    const design_t * d;
    int   item;
    float lo, H, pitch_k, space;
    int   nw;
    const float * ww;                      /* word widths at scale 1 */
    float cx[AREA_LINES], base[AREA_LINES], room[AREA_LINES];
    int   brk[AREA_LINES + 1];             /* line i = words brk[i] .. brk[i+1]-1 */
    int   nl;
} area_fit_t;

/* `n` lines at `cap`, centred in the shape; false when the block is taller
 * than the shape or a line would leave it */
static bool area_geometry(area_fit_t * a, float cap, int n)
{
    float pitch = cap * a->pitch_k, bh = cap + (n - 1) * pitch;
    if(bh > a->H + 0.5f) return false;
    float top = a->lo + (a->H - bh) / 2;
    for(int i = 0; i < n; i++) {
        a->base[i] = top + i * pitch + cap;
        a->room[i] = area_width(a->d, a->item, a->base[i] - cap, a->base[i], &a->cx[i]);
        if(a->room[i] <= 0) return false;
    }
    return true;
}

/* Words filled greedily into up to `n` lines at `scale` (greedy is enough:
 * a fuller line never leaves the next ones more to carry); returns how many
 * words found a place, a->nl the lines used. */
static int area_fill(area_fit_t * a, float scale, int n)
{
    int w = 0;
    a->nl = 0;
    for(int i = 0; i < n; i++) {
        a->brk[i] = w;
        float used = 0;
        while(w < a->nw) {
            float add = (used > 0 ? a->space : 0) + a->ww[w];
            if((used + add) * scale > a->room[i] + 0.5f) break;
            used += add;
            w++;
        }
        if(w == a->brk[i]) break;
        a->nl = i + 1;
    }
    a->brk[a->nl] = w;
    return w;
}

/* Fit `text` into the item's shape and draw it (see MAX_AREAS). Sizes go
 * down a ladder from the shape's full height; at each size one, two then
 * three lines are tried. At the floor: as many lines as the shape takes,
 * the last one trimmed with an ellipsis. */
static void render_area(uint32_t * out, const design_t * d, int item, int fi, bool caps, const char * text, uint32_t ink)
{
    if(fi < 0 || !text || !text[0]) return;
    const font_t * f = &fonts[fi];
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    if(caps) upper_ascii(buf);

    area_fit_t a = { .d = d, .item = item, .lo = 1e9f, .pitch_k = caps ? 1.22f : 1.42f };
    float hi = -1e9f;
    for(int i = 0; i < d->area_count; i++) if(d->areas[i].item == item) {
        if(d->areas[i].y0 < a.lo) a.lo = d->areas[i].y0;
        if(d->areas[i].y1 > hi) hi = d->areas[i].y1;
    }
    a.H = hi - a.lo;
    if(a.H <= 0) return;

    enum { MAXW = 48 };
    const char * ws[MAXW];
    int wl[MAXW];
    float ww[MAXW];
    for(char * p = buf; *p && a.nw < MAXW;) {
        while(*p == ' ') p++;
        if(!*p) break;
        char * q = p;
        while(*q && *q != ' ') q++;
        char save = *q;
        *q = 0;
        ws[a.nw] = p; wl[a.nw] = (int)(q - p); ww[a.nw] = text_width(f, p, 1.0f);
        *q = save;
        a.nw++;
        p = q;
    }
    if(a.nw == 0) return;
    a.ww = ww;
    a.space = text_width(f, " ", 1.0f);
    float floor_cap = AREA_CAP_FLOOR * (float)box;

    float cap = a.H;
    bool found = false;
    for(;;) {
        bool at_floor = cap <= floor_cap;
        if(at_floor) cap = floor_cap;
        for(int n = 1; n <= AREA_LINES && !found; n++)
            if(area_geometry(&a, cap, n) && area_fill(&a, cap / f->cap_units, n) == a.nw) found = true;
        if(found || at_floor) break;
        cap *= AREA_STEP;
    }
    if(!found) {
        int n = AREA_LINES;
        while(n > 0 && !area_geometry(&a, cap, n)) n--;
        if(n > 0) area_fill(&a, cap / f->cap_units, n);
        if(n == 0 || a.nl == 0) {                  /* one word wider than the shape: a single trimmed line */
            line_t ln = { 0 };
            ln.cap = floor_cap;
            ln.baseline = a.lo + (a.H + floor_cap) / 2;
            ln.room = area_width(d, item, ln.baseline - floor_cap, ln.baseline, &ln.cx);
            if(ln.room > 0) render_line(out, &ln, fi, false, buf, ink);
            return;
        }
    }

    float scale = cap / f->cap_units;
    for(int i = 0; i < a.nl; i++) {
        char line[512];
        size_t o = 0;
        int end = i == a.nl - 1 ? a.nw : a.brk[i + 1];    /* the last line takes any words left over */
        for(int k = a.brk[i]; k < end && o + (size_t)wl[k] + 2 < sizeof(line); k++) {
            if(o) line[o++] = ' ';
            memcpy(line + o, ws[k], (size_t)wl[k]);
            o += (size_t)wl[k];
        }
        line[o] = 0;
        float lw = text_width(f, line, scale);
        if(lw > a.room[i] + 0.5f) {                      /* leftovers at the floor: trimmed at the same size */
            line_t ln = { a.cx[i], a.base[i], cap, a.room[i] };
            render_line_min(out, &ln, fi, false, line, ink, 1.0f);
            continue;
        }
        draw_text(out, f, line, scale, a.cx[i] - lw / 2, a.base[i], ink);
    }
}

static void render_lines(uint32_t * out, const design_t * d, const char * const * texts, uint32_t ink, int album_font, bool album_caps, bool all_lines)
{
    for(int l = 0; l < d->line_count; l++) {
        int fi = line_font[l];
        bool caps = line_caps[l];
        if((l == 1 || all_lines) && album_font >= 0) { fi = album_font; caps = album_caps; }   /* line 2 is the album's; or every line (a mixtape's hand, 0.21b) */
        render_line(out, &d->lines[l], fi, caps, texts[l], ink);
    }
    for(int item = 0; item < 3 && d->area_count > 0; item++) {
        int fi = line_font[item];
        bool caps = line_caps[item];
        if((item == 1 || all_lines) && album_font >= 0) { fi = album_font; caps = album_caps; }
        render_area(out, d, item, fi, caps, texts[item], ink);
    }
}

uint8_t * placeholder_render_design(const char * design, const placeholder_tint_t * tints, int ntints, uint32_t fallback,
                                    uint32_t ink, const char * album_font, bool album_caps,
                                    const char * artist, const char * album, const char * title, int * w, int * h)
{
    if(!ready) return NULL;
    const design_t * d = design_get(design);
    if(!d) return NULL;
    uint32_t * out = malloc((size_t)box * box * 4);
    if(!out) return NULL;
    uint32_t zone_rgb[MAX_ZONES];
    tints_to_zones(d, tints, ntints, fallback, zone_rgb);
    tint(out, d, zone_rgb);
    const char * texts[MAX_LINES] = { artist ? artist : "", album ? album : "", title ? title : "", "" };
    bool all = album_font && album_font[0] == '*';                       /* "*<file>": that face on every line */
    if(all) album_font++;
    int af = album_font && album_font[0] ? font_index(album_font) : -1;
    render_lines(out, d, texts, ink, af, album_caps, all);
    *w = *h = box;
    return (uint8_t *)out;
}

void placeholder_prefer(const char * const * uris, int nuris, const char * artist, const char * album,
                        const placeholder_tint_t * tints, int ntints, uint32_t fallback,
                        uint32_t ink, const char * album_font, bool album_caps)
{
    if(!ready) return;
    pthread_mutex_lock(&prefer_lock);
    for(int i = 0; i < prefer_nuris; i++) free(prefer_uris[i]);
    free(prefer_uris);
    for(int i = 0; i < prefer_ntints; i++) free((char *)prefer_tints[i].zone);
    prefer_ntints = 0;
    prefer_uris = NULL;
    prefer_nuris = 0;
    have_prefer = artist && album;
    if(have_prefer) {
        snprintf(prefer_artist, sizeof(prefer_artist), "%s", artist);
        snprintf(prefer_album, sizeof(prefer_album), "%s", album);
        if(uris && nuris > 0 && (prefer_uris = calloc((size_t)nuris, sizeof(char *))) != NULL) {
            for(int i = 0; i < nuris; i++) prefer_uris[i] = uris[i] ? strdup(uris[i]) : NULL;
            prefer_nuris = nuris;
        }
        tints_to_zones(main_design, tints, ntints, fallback, prefer_rgb);
        prefer_ntints = ntints < 8 ? ntints : 8;
        for(int i = 0; i < prefer_ntints; i++) { prefer_tints[i] = tints[i]; prefer_tints[i].zone = tints[i].zone ? strdup(tints[i].zone) : NULL; }
        prefer_fallback = fallback;
        for(int z = 0; z < main_design->zone_count; z++) prefer_set[z] = true;
        prefer_ink = ink;
        snprintf(prefer_font, sizeof(prefer_font), "%s", album_font ? album_font : "");
        prefer_caps = album_caps;
    }
    pthread_mutex_unlock(&prefer_lock);
}

const char * placeholder_line_font(int line, bool * caps)
{
    if(!ready || line < 1 || line > MAX_LINES || line_font[line - 1] < 0) return NULL;
    if(caps) *caps = line_caps[line - 1];
    return fonts[line_font[line - 1]].file;
}

uint8_t * placeholder_render(const char * uri, const char * artist, const char * album, const char * title, int * w, int * h)
{
    if(!ready) return NULL;
    uint32_t * out = malloc((size_t)box * box * 4);
    if(!out) return NULL;
    const char * texts[MAX_LINES] = { artist, album, title, "" };

    /* the album page's colours for this album, when it set them (0.17h):
     * a track of it by path, else by its tags */
    pthread_mutex_lock(&prefer_lock);
    bool preferred = false;
    const track_pref_t * tp = NULL;
    if(uri && uri[0] && track_prefs) for(int i = 0; i < track_pref_n; i++) if(strcmp(track_prefs[i].uri, uri) == 0) { tp = &track_prefs[i]; break; }
    if(tp) preferred = true;
    else if(have_prefer) {
        if(uri && uri[0]) for(int i = 0; i < prefer_nuris && !preferred; i++) preferred = prefer_uris[i] && strcmp(prefer_uris[i], uri) == 0;
        if(!preferred && artist && album && artist[0] && album[0])
            preferred = strcasecmp(artist, prefer_artist) == 0 && strcasecmp(album, prefer_album) == 0;
    }
    /* the tape or the record, never the same twice running (Ian, 17 Oct:
     * the record joins the rotation, from an album's page too) */
    static int last_kind = -1;
    const design_t * rec = design_get("record");
    int kind = 0;
    if(rec) { kind = rand() % 2; if(kind == last_kind) kind ^= 1; }
    last_kind = kind;

    if(preferred) {
        uint32_t zone_rgb[MAX_ZONES];
        placeholder_tint_t t[8];
        char zones[8][16];
        int nt;
        uint32_t ink, fb;
        char font[64];
        bool caps;
        if(tp) {
            nt = tp->ntints;
            for(int i = 0; i < nt; i++) { t[i].rgb = tp->tints[i].rgb; snprintf(zones[i], sizeof(zones[i]), "%s", tp->zones[i]); t[i].zone = zones[i]; }
            fb = tp->fallback; ink = tp->ink; caps = tp->caps;
            snprintf(font, sizeof(font), "%s", tp->font);
            tints_to_zones(main_design, t, nt, fb, zone_rgb);
        }
        else {
            memcpy(zone_rgb, prefer_rgb, sizeof(zone_rgb));
            ink = prefer_ink;
            snprintf(font, sizeof(font), "%s", prefer_font);
            caps = prefer_caps;
            nt = prefer_ntints;
            memcpy(t, prefer_tints, sizeof(t));
            fb = prefer_fallback;
        }
        pthread_mutex_unlock(&prefer_lock);
        if(kind == 1) {
            /* the page's own record colours (its disc, centre, background) */
            uint32_t zr[MAX_ZONES];
            tints_to_zones(rec, t, nt, fb, zr);
            tint(out, rec, zr);
            const char * rtexts[MAX_LINES] = { artist, title, "", "" };
            render_lines(out, rec, rtexts, ink, -1, false, false);
        }
        else {
            tint(out, main_design, zone_rgb);
            render_lines(out, main_design, texts, ink, font[0] ? font_index(font) : -1, caps, false);
        }
        *w = *h = box;
        return (uint8_t *)out;
    }
    pthread_mutex_unlock(&prefer_lock);

    int i;
    if(last_way < 0 || way_count < 2) i = rand() % way_count;
    else { i = rand() % (way_count - 1); if(i >= last_way) i++; }   /* never the previous pick */
    last_way = i;
    const colourway_t * way = &ways[i];

    /* the record carries the artist and the song (its two slots), its disc
     * the tape's shell colour, black one in five */
    if(kind == 1) {
        int zb = zone_index(main_design, "background"), zl = zone_index(main_design, "label"), zs = zone_index(main_design, "shell");
        uint32_t bg = zb >= 0 ? way->zone_rgb[zb] : 0x202020, label = zl >= 0 ? way->zone_rgb[zl] : way->ink, shell = zs >= 0 ? way->zone_rgb[zs] : label;
        bool black = rand() % 5 == 0;
        placeholder_tint_t t[6] = { {"background", bg}, {"centre", label}, {"disc", black ? 0x141414 : shell}, {"groove", black ? 0x141414 : shell}, {"spindle", label}, {"sleeve", shell} };
        uint32_t zr[MAX_ZONES];
        tints_to_zones(rec, t, 6, bg, zr);
        tint(out, rec, zr);
        const char * rtexts[MAX_LINES] = { artist, title, "", "" };
        render_lines(out, rec, rtexts, way->ink, -1, false, false);
        *w = *h = box;
        return (uint8_t *)out;
    }

    tint(out, main_design, way->zone_rgb);
    render_lines(out, main_design, texts, way->ink, -1, false, false);

    *w = *h = box;
    return (uint8_t *)out;
}
