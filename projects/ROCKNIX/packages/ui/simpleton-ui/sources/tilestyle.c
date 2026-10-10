/*
 * SimpletonOS UI - the look of the grid's no-art tiles (implementation).
 * See tilestyle.h.
 */
#include "tilestyle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAL_MAX     128       /* palette colours kept */
#define PAIR_MAX    512       /* (bg, ink) pairs in the theme / generate pools */
#define FONT_MAX    16
#define INKS_MAX    24        /* inks remembered per mix background */
#define GEN_COUNT   40        /* colourways invented per visit */
#define CONTRAST_DEFAULT  3.5   /* without a tilecontrast line */
#define BG_MIN_LUM  0.05      /* darker than this is an ink, not a tile: it vanishes on the dark UI */
#define NEAR_SAME   28.0      /* redmean distance under which two colours count as one */
#define TOO_CLOSE   70.0      /* neighbouring tiles closer than this are skipped */

typedef struct { uint32_t bg, ink; } pair_t;

static tile_colours_t mode;
static uint32_t pal[PAL_MAX];          /* every colour, deduplicated (mix, generate) */
static int      npal;
static uint32_t set_col[PAL_MAX];      /* tilepalette colours, in file order... */
static int      set_of[PAL_MAX];       /* ...and which set each belongs to       */
static int      nset_col, nsets;
static pair_t   theme_pairs[PAIR_MAX];
static int      ntheme;
static char     fonts[FONT_MAX][64];
static bool     font_caps[FONT_MAX];
static int      nfonts;
static double   min_contrast;          /* tilecontrast: how readable an ink must be */
static uint32_t cream;                 /* tilecream: the shared cream ink, 0 = none  */
static bool     has_cream;
static int      w_pal, w_cream, w_dark; /* tileinks: how often each kind of ink is drawn */

/* ----------------------------------------------------------- colour */

static double chan_lin(int c)
{
    double s = c / 255.0;
    return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    return 0.2126 * chan_lin((rgb >> 16) & 255) + 0.7152 * chan_lin((rgb >> 8) & 255) + 0.0722 * chan_lin(rgb & 255);
}

double tilestyle_contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a), lb = luminance(b);
    if(la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

/* "redmean": a cheap perceptual distance, 0..~765 */
static double distance(uint32_t a, uint32_t b)
{
    int r1 = (a >> 16) & 255, g1 = (a >> 8) & 255, b1 = a & 255;
    int r2 = (b >> 16) & 255, g2 = (b >> 8) & 255, b2 = b & 255;
    double rm = (r1 + r2) / 2.0;
    double dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

static void to_hsl(uint32_t rgb, double * h, double * s, double * l)
{
    double r = ((rgb >> 16) & 255) / 255.0, g = ((rgb >> 8) & 255) / 255.0, b = (rgb & 255) / 255.0;
    double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b)), d = mx - mn;
    *l = (mx + mn) / 2;
    if(d < 1e-9) { *h = 0; *s = 0; return; }
    *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if(mx == r) *h = fmod((g - b) / d + 6, 6);
    else if(mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h *= 60;
}

static double hue2(double p, double q, double t)
{
    if(t < 0) t += 1;
    if(t > 1) t -= 1;
    if(t < 1.0 / 6) return p + (q - p) * 6 * t;
    if(t < 0.5) return q;
    if(t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static uint32_t from_hsl(double h, double s, double l)
{
    h = fmod(fmod(h, 360) + 360, 360) / 360;
    if(s < 0) s = 0;
    if(s > 1) s = 1;
    if(l < 0) l = 0;
    if(l > 1) l = 1;
    double r, g, b;
    if(s == 0) r = g = b = l;
    else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue2(p, q, h + 1.0 / 3); g = hue2(p, q, h); b = hue2(p, q, h - 1.0 / 3);
    }
    return ((uint32_t)lround(r * 255) << 16) | ((uint32_t)lround(g * 255) << 8) | (uint32_t)lround(b * 255);
}

/* ----------------------------------------------------------- random */

typedef struct { uint32_t s; } rng_t;

static uint32_t rnd(rng_t * r)
{
    uint32_t x = r->s ? r->s : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}

static double rndf(rng_t * r) { return (rnd(r) >> 8) / 16777216.0; }
static double rndr(rng_t * r, double a, double b) { return a + (b - a) * rndf(r); }

static void shuffle(int * a, int n, rng_t * r)
{
    for(int i = n - 1; i > 0; i--) { int j = (int)(rnd(r) % (uint32_t)(i + 1)); int t = a[i]; a[i] = a[j]; a[j] = t; }
}

/* ----------------------------------------------------------- loading */

static bool parse_hex(const char * s, uint32_t * out)
{
    if(*s == '#') s++;
    char * end;
    unsigned long v = strtoul(s, &end, 16);
    if(end - s != 6) return false;
    *out = (uint32_t)v;
    return true;
}

static void pal_add(uint32_t c)
{
    for(int i = 0; i < npal; i++) if(distance(pal[i], c) < NEAR_SAME) return;
    if(npal < PAL_MAX) pal[npal++] = c;
}

void tilestyle_load(const char * theme_path)
{
    mode = TILE_COLOURS_THEME;
    npal = ntheme = nfonts = nset_col = nsets = 0;
    min_contrast = CONTRAST_DEFAULT;
    has_cream = false;
    w_pal = 100; w_cream = 0; w_dark = 0;
    FILE * f = theme_path ? fopen(theme_path, "r") : NULL;
    char line[512];
    while(f && fgets(line, sizeof(line), f)) {
        char * semi = strchr(line, ';');
        if(semi) *semi = 0;
        char a[64], b[64];
        if(sscanf(line, "tilecolours %63s", a) == 1) {
            if(strcmp(a, "palettes") == 0) mode = TILE_COLOURS_PALETTES;
            else if(strcmp(a, "mix") == 0) mode = TILE_COLOURS_MIX;
            else if(strcmp(a, "generate") == 0) mode = TILE_COLOURS_GENERATE;
            else mode = TILE_COLOURS_THEME;
        }
        else if(strncmp(line, "tilecontrast", 12) == 0) {
            double v = atof(line + 12);
            if(v >= 1.0 && v <= 21.0) min_contrast = v;
        }
        else if(sscanf(line, "tilecream %63s", a) == 1) {
            has_cream = parse_hex(a, &cream);
        }
        else if(strncmp(line, "tileinks", 8) == 0) {
            char * save;
            for(char * tok = strtok_r(line + 8, " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
                int v;
                if(sscanf(tok, "palette=%d", &v) == 1) w_pal = v < 0 ? 0 : v;
                else if(sscanf(tok, "cream=%d", &v) == 1) w_cream = v < 0 ? 0 : v;
                else if(sscanf(tok, "dark=%d", &v) == 1) w_dark = v < 0 ? 0 : v;
            }
        }
        else if(strncmp(line, "tilepalette", 11) == 0) {
            char * save;
            char * tok = strtok_r(line + 11, " \t\n", &save);             /* the set's name */
            bool any = false;
            for(tok = tok ? strtok_r(NULL, " \t\n", &save) : NULL; tok; tok = strtok_r(NULL, " \t\n", &save)) {
                uint32_t rgb;
                if(!parse_hex(tok, &rgb)) continue;
                pal_add(rgb);
                if(nset_col < PAL_MAX) { set_col[nset_col] = rgb; set_of[nset_col] = nsets; nset_col++; any = true; }
            }
            if(any) nsets++;
        }
        else if(sscanf(line, "tilefont %63s %63s", a, b) >= 1 && strncmp(line, "tilefont", 8) == 0) {
            if(nfonts < FONT_MAX) {
                snprintf(fonts[nfonts], sizeof(fonts[0]), "%s", a);
                font_caps[nfonts] = strcmp(b, "caps") == 0;
                nfonts++;
            }
        }
        else if(strncmp(line, "colourway", 9) == 0 || strncmp(line, "tileway", 7) == 0) {
            bool tileway = line[0] == 't';
            uint32_t label = 0, ink = 0, bg = 0;
            bool has_label = false, has_ink = false, has_bg = false;
            char * save;
            for(char * tok = strtok_r(line + (tileway ? 7 : 9), " \t\n", &save); tok; tok = strtok_r(NULL, " \t\n", &save)) {
                char * eq = strchr(tok, '=');
                uint32_t rgb;
                if(!eq || !parse_hex(eq + 1, &rgb)) continue;
                *eq = 0;
                pal_add(rgb);
                if(strcmp(tok, "text") == 0 || strcmp(tok, "ink") == 0) { ink = rgb; has_ink = true; }
                else if(strcmp(tok, "label") == 0) { label = rgb; has_label = true; }
                else if(strcmp(tok, "bg") == 0) { bg = rgb; has_bg = true; }
            }
            uint32_t back = tileway ? bg : label;
            if((tileway ? has_bg : has_label) && has_ink && ntheme < PAIR_MAX)
                theme_pairs[ntheme++] = (pair_t){ back, ink };
        }
    }
    if(f) fclose(f);
    fprintf(stderr, "simpleton-ui: tiles: colours %s, %d colourways, %d sets (%d colours), %d palette colours, "
            "contrast %.1f, inks palette %d cream %d dark %d, %d fonts\n",
            tilestyle_mode_name(), ntheme, nsets, nset_col, npal, min_contrast, w_pal, has_cream ? w_cream : 0, w_dark, nfonts);
}

tile_colours_t tilestyle_mode(void) { return mode; }

const char * tilestyle_mode_name(void)
{
    return mode == TILE_COLOURS_PALETTES ? "palettes" : mode == TILE_COLOURS_MIX ? "mix" : mode == TILE_COLOURS_GENERATE ? "generate" : "theme";
}

int tilestyle_font_count(void) { return nfonts; }

const char * tilestyle_font(int i, bool * caps)
{
    if(i < 0 || i >= nfonts) return NULL;
    if(caps) *caps = font_caps[i];
    return fonts[i];
}

/* ----------------------------------------------------------- pools */

/* A tile colour and the inks it may take, by kind (Ian, 9 Oct: "a lot with
 * colours from their palette", cream common, some off-black; Archer Ave. is
 * about vibe more than contrast, so the floor is the theme's tilecontrast). */
typedef struct {
    uint32_t bg;
    uint32_t col[INKS_MAX];  int ncol;     /* palette colours that read on it */
    uint32_t dark[INKS_MAX]; int ndark;    /* off-blacks that read on it      */
    bool     cream_ok;                     /* the cream reads, and it isn't pastel */
} mixbg_t;

static bool is_dark(uint32_t c) { return luminance(c) < BG_MIN_LUM; }

static bool reads(uint32_t bg, uint32_t ink)
{
    return tilestyle_contrast(bg, ink) >= min_contrast && distance(bg, ink) >= TOO_CLOSE;
}

/* Fill m's inks. `same_set`: the set to draw palette colours from, -1 any.
 * Off-blacks come from the same set when it has one that reads, else any. */
static void collect_inks(mixbg_t * m, int same_set)
{
    m->ncol = m->ndark = 0;
    if(same_set >= 0) {
        for(int j = 0; j < nset_col; j++) {
            if(set_of[j] != same_set || !reads(m->bg, set_col[j])) continue;
            if(is_dark(set_col[j])) { if(m->ndark < INKS_MAX) m->dark[m->ndark++] = set_col[j]; }
            else if(m->ncol < INKS_MAX) m->col[m->ncol++] = set_col[j];
        }
    }
    if(m->ncol == 0)                     /* any set, also when its own set has nothing that reads */
        for(int j = 0; j < npal && m->ncol < INKS_MAX; j++)
            if(!is_dark(pal[j]) && reads(m->bg, pal[j])) m->col[m->ncol++] = pal[j];
    if(m->ndark == 0)
        for(int j = 0; j < npal && m->ndark < INKS_MAX; j++)
            if(is_dark(pal[j]) && reads(m->bg, pal[j])) m->dark[m->ndark++] = pal[j];
    double h, s, l;
    to_hsl(m->bg, &h, &s, &l);
    m->cream_ok = has_cream && l < 0.80 && reads(m->bg, cream);
}

/* The colour's own hue taken deep or cream, whichever reads better: the
 * last resort when nothing in the palette reads on it. */
static uint32_t own_ink(uint32_t bg)
{
    double h, s, l;
    to_hsl(bg, &h, &s, &l);
    uint32_t deep = from_hsl(h, s < 0.25 ? 0.25 : s > 0.7 ? 0.7 : s, 0.16);
    uint32_t pale = from_hsl(h, 0.5, 0.94);
    return tilestyle_contrast(bg, deep) >= tilestyle_contrast(bg, pale) ? deep : pale;
}

/* Draw one ink by the theme's weights, over the kinds this tile can take. */
static uint32_t pick_ink(const mixbg_t * m, rng_t * r)
{
    int wp = m->ncol ? w_pal : 0, wc = m->cream_ok ? w_cream : 0, wd = m->ndark ? w_dark : 0;
    int tot = wp + wc + wd;
    if(tot <= 0) {                       /* the weights rule out everything it can take */
        if(m->ncol) return m->col[rnd(r) % (uint32_t)m->ncol];
        if(m->cream_ok) return cream;
        if(m->ndark) return m->dark[rnd(r) % (uint32_t)m->ndark];
        return own_ink(m->bg);
    }
    int x = (int)(rnd(r) % (uint32_t)tot);
    if(x < wp) return m->col[rnd(r) % (uint32_t)m->ncol];
    if(x < wp + wc) return cream;
    return m->dark[rnd(r) % (uint32_t)m->ndark];
}

/* mix: any palette colour as the tile, inks from anywhere */
static int build_mix(mixbg_t * out)
{
    int n = 0;
    for(int i = 0; i < npal; i++) {
        if(is_dark(pal[i])) continue;
        out[n].bg = pal[i];
        collect_inks(&out[n], -1);
        n++;
    }
    return n;
}

/* palettes: every set colour, inked from its own set */
static int build_palettes(mixbg_t * out)
{
    int n = 0;
    for(int i = 0; i < nset_col; i++) {
        if(is_dark(set_col[i])) continue;
        out[n].bg = set_col[i];
        collect_inks(&out[n], set_of[i]);
        n++;
    }
    return n;
}

/* generate: colourways invented around the palette */
static int build_generated(mixbg_t * out, rng_t * r)
{
    double hues[PAL_MAX];
    int nh = 0;
    double smin = 1, smax = 0, lmin = 1, lmax = 0;
    for(int i = 0; i < npal; i++) {
        double h, s, l;
        to_hsl(pal[i], &h, &s, &l);
        if(s < 0.15 || l < 0.12 || l > 0.92) continue;        /* greys, near-blacks, creams: no hue to learn */
        hues[nh++] = h;
        if(s < smin) smin = s;
        if(s > smax) smax = s;
        if(l < lmin) lmin = l;
        if(l > lmax) lmax = l;
    }
    if(nh == 0) { hues[nh++] = 30; smin = 0.35; smax = 0.75; lmin = 0.35; lmax = 0.75; }
    if(lmin < 0.25) lmin = 0.25;
    if(lmax > 0.82) lmax = 0.82;
    if(lmax < lmin) lmax = lmin;

    int n = 0;
    for(int tries = 0; n < GEN_COUNT && tries < GEN_COUNT * 20; tries++) {
        double h = rndf(r) < 0.8 ? hues[rnd(r) % (uint32_t)nh] + rndr(r, -28, 28) : rndr(r, 0, 360);
        uint32_t bg = from_hsl(h, rndr(r, smin, smax), rndr(r, lmin, lmax));
        bool dup = false;
        for(int i = 0; i < n; i++) if(distance(out[i].bg, bg) < NEAR_SAME) { dup = true; break; }
        if(dup) continue;
        out[n].bg = bg;                    /* inked like mix: from the palette, cream, off-black */
        collect_inks(&out[n], -1);
        n++;
    }
    return n;
}

/* ----------------------------------------------------------- dealing */

/* A deck of `count` cards, reshuffled each time through. take() hands out
 * the first card that `ok` accepts (searching the rest of the deck), and
 * swaps it to the front so the skipped ones come up next. */
typedef struct { int * cards; int count, pos; rng_t * r; } deck_t;

static void deck_init(deck_t * d, int count, rng_t * r)
{
    d->cards = malloc(sizeof(int) * (size_t)(count > 0 ? count : 1));
    d->count = d->cards ? count : 0;
    d->pos = 0;
    d->r = r;
    for(int i = 0; i < d->count; i++) d->cards[i] = i;
    shuffle(d->cards, d->count, r);
}

static int deck_take(deck_t * d, bool (*ok)(int card, const void * ctx), const void * ctx)
{
    if(d->count == 0) return 0;
    if(d->pos >= d->count) { shuffle(d->cards, d->count, d->r); d->pos = 0; }
    int pick = d->pos;
    for(int k = 0; k < d->count; k++) {
        int idx = d->pos + k;
        if(idx >= d->count) break;                  /* only this pass's remaining cards */
        if(ok(d->cards[idx], ctx)) { pick = idx; break; }
    }
    int t = d->cards[d->pos]; d->cards[d->pos] = d->cards[pick]; d->cards[pick] = t;
    return d->cards[d->pos++];
}

typedef struct { const uint32_t * bgs; uint32_t left, up; bool has_left, has_up; } colour_ctx_t;

static bool colour_ok(int card, const void * vctx)
{
    const colour_ctx_t * c = vctx;
    uint32_t bg = c->bgs[card];
    if(c->has_left && distance(bg, c->left) < TOO_CLOSE) return false;
    if(c->has_up && distance(bg, c->up) < TOO_CLOSE) return false;
    return true;
}

typedef struct { int left, up; } font_ctx_t;

static bool font_ok(int card, const void * vctx)
{
    const font_ctx_t * c = vctx;
    return card != c->left && card != c->up;
}

void tilestyle_deal(unsigned seed, int n, int cols, const bool * noart, tile_style_t * out)
{
    if(n <= 0 || !out) return;
    memset(out, 0, sizeof(tile_style_t) * (size_t)n);
    if(cols < 1) cols = 1;
    rng_t r = { seed * 2654435761u + 0x51ED270Bu };

    /* the pool for this visit: backgrounds, and how to find an ink */
    static mixbg_t mix[PAL_MAX > GEN_COUNT ? PAL_MAX : GEN_COUNT];
    static uint32_t bgs[PAIR_MAX];
    int pool = 0;
    tile_colours_t used = mode;
    if(mode == TILE_COLOURS_PALETTES) { pool = build_palettes(mix); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    else if(mode == TILE_COLOURS_MIX) { pool = build_mix(mix); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    else if(mode == TILE_COLOURS_GENERATE) { pool = build_generated(mix, &r); for(int i = 0; i < pool; i++) bgs[i] = mix[i].bg; }
    if(pool == 0) {                                   /* theme mode, or the others found nothing */
        used = TILE_COLOURS_THEME;
        pool = ntheme;
        for(int i = 0; i < pool; i++) bgs[i] = theme_pairs[i].bg;
    }

    deck_t cd, fd;
    deck_init(&cd, pool, &r);
    deck_init(&fd, nfonts, &r);

    for(int e = 0; e < n; e++) {
        if(!noart[e]) continue;
        int l = (e % cols > 0 && noart[e - 1]) ? e - 1 : -1;
        int u = (e >= cols && noart[e - cols]) ? e - cols : -1;
        if(pool > 0) {
            colour_ctx_t cc = { bgs, l >= 0 ? out[l].bg : 0, u >= 0 ? out[u].bg : 0, l >= 0, u >= 0 };
            int card = deck_take(&cd, colour_ok, &cc);
            out[e].bg = bgs[card];
            if(used == TILE_COLOURS_THEME) out[e].ink = theme_pairs[card].ink;
            else out[e].ink = pick_ink(&mix[card], &r);
        }
        else { out[e].bg = 0x333333; out[e].ink = 0xEEEEEE; }
        if(nfonts > 0) {
            font_ctx_t fc = { l >= 0 ? out[l].font : -1, u >= 0 ? out[u].font : -1 };
            out[e].font = (uint8_t)deck_take(&fd, font_ok, &fc);
        }
    }
    free(cd.cards);
    free(fd.cards);
}
