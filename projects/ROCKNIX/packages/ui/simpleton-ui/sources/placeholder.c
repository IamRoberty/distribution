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

#include <ctype.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define MAX_FONTS  8

#define SHRINK_MIN 0.55f     /* a long line shrinks to 55 % before it is trimmed */

/* ------------------------------------------------------------- types */

typedef struct { uint8_t * px; int w, h; } gray_t;

typedef struct { char name[32]; gray_t mask; } zone_t;

typedef struct { float cx, baseline, cap, room; } line_t;     /* output px */

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

/* ------------------------------------------------------------- state */

static char  share_dir[256];
static int   box;

static gray_t base;
static zone_t zones[MAX_ZONES];
static int    zone_count;
static line_t lines[MAX_LINES];
static int    line_count;

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

static int zone_index(const char * name)
{
    for(int i = 0; i < zone_count; i++) if(strcmp(zones[i].name, name) == 0) return i;
    return -1;
}

static int font_index(const char * file)
{
    for(int i = 0; i < font_count; i++) if(strcmp(fonts[i].file, file) == 0) return i;
    if(font_count == MAX_FONTS) return -1;
    font_t * f = &fonts[font_count];
    char path[512];
    snprintf(path, sizeof(path), "%s/fonts/%s", share_dir, file);
    size_t len;
    f->data = read_file(path, &len);
    if(!f->data || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        fprintf(stderr, "simpleton-ui: placeholder: cannot load font %s\n", path);
        free(f->data); f->data = NULL;
        return -1;
    }
    int x0, y0, x1, y1;
    f->cap_units = stbtt_GetCodepointBox(&f->info, 'H', &x0, &y0, &x1, &y1) ? y1 : 700;
    snprintf(f->file, sizeof(f->file), "%s", file);
    return font_count++;
}

/* layout.txt: "size W H" then "text cx baseline cap room" per line. */
static bool load_layout(const char * design)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/placeholder/%s/layout.txt", share_dir, design);
    FILE * f = fopen(path, "r");
    if(!f) return false;
    float mw = 0, mh = 0;
    char line[256];
    line_count = 0;
    while(fgets(line, sizeof(line), f)) {
        float a, b, c, d;
        if(sscanf(line, "size %f %f", &a, &b) == 2) { mw = a; mh = b; }
        else if(sscanf(line, "text %f %f %f %f", &a, &b, &c, &d) == 4 && line_count < MAX_LINES && mw > 0) {
            float s = (float)box / mw;
            lines[line_count++] = (line_t){ a * s, b * s, c * s, d * s };
        }
    }
    fclose(f);
    (void)mh;
    return mw > 0 && line_count > 0;
}

static bool load_design(const char * design)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/placeholder/%s/base.png", share_dir, design);
    if(!load_gray(path, &base) || !resample_gray(&base, box, box)) return false;

    static const char * const zone_names[] = { "background", "shell", "label", "disc", "sleeve", "centre", NULL };
    zone_count = 0;
    for(int i = 0; zone_names[i] && zone_count < MAX_ZONES; i++) {
        snprintf(path, sizeof(path), "%s/placeholder/%s/mask-%s.png", share_dir, design, zone_names[i]);
        zone_t * z = &zones[zone_count];
        if(!load_gray(path, &z->mask)) continue;
        if(!resample_gray(&z->mask, box, box)) return false;
        snprintf(z->name, sizeof(z->name), "%s", zone_names[i]);
        zone_count++;
    }
    return zone_count > 0 && load_layout(design);
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
    if(!design[0] || !load_design(design)) { fclose(f); return false; }

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
                int z = zone_index(tok);
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
    for(; *s; s++) if((unsigned char)*s < 0x80) *s = (char)toupper((unsigned char)*s);
}

/* Centre `text` on the line: shrink to SHRINK_MIN if too wide, then trim
 * with an ellipsis. */
static void render_line(uint32_t * out, const line_t * ln, int fi, bool caps, const char * text, uint32_t ink)
{
    if(fi < 0 || !text || !text[0]) return;
    const font_t * f = &fonts[fi];
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", text);
    if(caps) upper_ascii(buf);

    float scale = ln->cap / f->cap_units;
    float w = text_width(f, buf, scale);
    if(w > ln->room) {
        float s2 = scale * ln->room / w;
        scale = s2 > scale * SHRINK_MIN ? s2 : scale * SHRINK_MIN;
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
static void tint(uint32_t * out, const colourway_t * way)
{
    size_t n = (size_t)box * box;
    for(size_t i = 0; i < n; i++) {
        unsigned sum = 0, r = 0, g = 0, b = 0;
        for(int z = 0; z < zone_count; z++) {
            unsigned m = zones[z].mask.px[i];
            if(!m) continue;
            sum += m;
            r += m * ((way->zone_rgb[z] >> 16) & 0xFF);
            g += m * ((way->zone_rgb[z] >> 8) & 0xFF);
            b += m * (way->zone_rgb[z] & 0xFF);
        }
        unsigned tot = sum > 255 ? sum : 255;             /* /255 keeps colour true where sum == 255 */
        unsigned gap = sum >= 255 ? 0 : 255 - sum;        /* uncovered part stays untinted */
        unsigned mr = r / tot + gap, mg = g / tot + gap, mb = b / tot + gap;   /* 0..255 multipliers */
        if(mr > 255) mr = 255;
        if(mg > 255) mg = 255;
        if(mb > 255) mb = 255;
        unsigned v = base.px[i];
        out[i] = 0xFF000000u | ((v * mr / 255) << 16) | ((v * mg / 255) << 8) | (v * mb / 255);
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
        fprintf(stderr, "simpleton-ui: placeholder: theme %s, %d colourways, %d zones, %d text lines, box %d\n",
                theme_name, way_count, zone_count, line_count, box);
    }
    else fprintf(stderr, "simpleton-ui: placeholder: assets not usable under %s, using plain placeholder\n", share_dir);
    return ready;
}

int placeholder_colourway_count(void) { return ready ? way_count : 0; }

bool placeholder_colourway(int i, uint32_t * label_rgb, uint32_t * ink_rgb)
{
    if(!ready || i < 0 || i >= way_count) return false;
    const colourway_t * w = &ways[i];
    int z = zone_index("label");
    if(label_rgb) *label_rgb = z >= 0 ? w->zone_rgb[z] : w->zone_rgb[0];
    if(ink_rgb) *ink_rgb = w->ink;
    return true;
}

const char * placeholder_line_font(int line, bool * caps)
{
    if(!ready || line < 1 || line > MAX_LINES || line_font[line - 1] < 0) return NULL;
    if(caps) *caps = line_caps[line - 1];
    return fonts[line_font[line - 1]].file;
}

uint8_t * placeholder_render(const char * artist, const char * album, const char * title, int * w, int * h)
{
    if(!ready) return NULL;
    uint32_t * out = malloc((size_t)box * box * 4);
    if(!out) return NULL;

    int i;
    if(last_way < 0 || way_count < 2) i = rand() % way_count;
    else { i = rand() % (way_count - 1); if(i >= last_way) i++; }   /* never the previous pick */
    last_way = i;
    const colourway_t * way = &ways[i];

    tint(out, way);
    const char * texts[MAX_LINES] = { artist, album, title, "" };
    for(int l = 0; l < line_count; l++) render_line(out, &lines[l], line_font[l], line_caps[l], texts[l], way->ink);

    *w = *h = box;
    return (uint8_t *)out;
}
