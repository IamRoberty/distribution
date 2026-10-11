/*
 * SimpletonOS - colours from a cover (implementation). See palette.h.
 */
#include "palette.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX   16384
#define BOXES        32
#define SHARE_MIN    0.0015       /* a colour below this share can't be the lettering */
#define NEAR         60.0         /* two colours closer than this count as one role */

/* ----------------------------------------------------------- colour maths */

static double chan_lin(int c)
{
    double s = c / 255.0;
    return s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
}

static double luminance(uint32_t rgb)
{
    return 0.2126 * chan_lin((rgb >> 16) & 255) + 0.7152 * chan_lin((rgb >> 8) & 255) + 0.0722 * chan_lin(rgb & 255);
}

double palette_contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a), lb = luminance(b);
    if(la < lb) { double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

static double dist(uint32_t a, uint32_t b)
{
    double dr = (double)((a >> 16) & 255) - (double)((b >> 16) & 255);
    double dg = (double)((a >> 8) & 255) - (double)((b >> 8) & 255);
    double db = (double)(a & 255) - (double)(b & 255);
    return sqrt(dr * dr + dg * dg + db * db);
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

static double sat_of(uint32_t c) __attribute__((unused));
static double sat_of(uint32_t c) { double h, s, l; to_hsl(c, &h, &s, &l); return s; }
static double light_of(uint32_t c) { double h, s, l; to_hsl(c, &h, &s, &l); return l; }
static double hue_of(uint32_t c) { double h, s, l; to_hsl(c, &h, &s, &l); return h; }
/* colourfulness as a plain channel spread, 0..1 (near-blacks stay near 0,
 * unlike HSL saturation) */
static double chroma_of(uint32_t c)
{
    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    int mx = r > g ? r : g; if(b > mx) mx = b;
    int mn = r < g ? r : g; if(b < mn) mn = b;
    return (mx - mn) / 255.0;
}

/* ----------------------------------------------------------- median cut */

typedef struct { uint8_t r, g, b; } px_t;
typedef struct { int from, to; } box_t;            /* a range of the sample array */

static int cmp_r(const void * a, const void * b) { return (int)((const px_t *)a)->r - (int)((const px_t *)b)->r; }
static int cmp_g(const void * a, const void * b) { return (int)((const px_t *)a)->g - (int)((const px_t *)b)->g; }
static int cmp_b(const void * a, const void * b) { return (int)((const px_t *)a)->b - (int)((const px_t *)b)->b; }

typedef struct { uint32_t rgb; int count; } entry_t;

static int cmp_count_desc(const void * a, const void * b) { return ((const entry_t *)b)->count - ((const entry_t *)a)->count; }

bool palette_extract(const uint8_t * xrgb, int w, int h, art_palette_t * out)
{
    memset(out, 0, sizeof(*out));
    if(!xrgb || w < 1 || h < 1) return false;
    /* sample on a grid */
    int step = 1;
    while((w / step) * (h / step) > SAMPLE_MAX) step++;
    int n = 0;
    px_t * s = malloc(sizeof(px_t) * (size_t)SAMPLE_MAX + 64);
    if(!s) return false;
    for(int y = 0; y < h; y += step)
        for(int x = 0; x < w && n < SAMPLE_MAX; x += step) {
            const uint8_t * p = xrgb + ((size_t)y * (size_t)w + (size_t)x) * 4;
            s[n].b = p[0]; s[n].g = p[1]; s[n].r = p[2];
            n++;
        }
    if(n < 4) { free(s); return false; }

    box_t boxes[BOXES];
    int nboxes = 1;
    boxes[0].from = 0; boxes[0].to = n;
    while(nboxes < BOXES) {
        /* the box with the widest channel range, split at its median */
        int best = -1, best_range = -1, best_ch = 0;
        for(int i = 0; i < nboxes; i++) {
            box_t * bx = &boxes[i];
            if(bx->to - bx->from < 2) continue;
            int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
            for(int k = bx->from; k < bx->to; k++) {
                int v[3] = { s[k].r, s[k].g, s[k].b };
                for(int c = 0; c < 3; c++) { if(v[c] < mn[c]) mn[c] = v[c]; if(v[c] > mx[c]) mx[c] = v[c]; }
            }
            for(int c = 0; c < 3; c++) {
                int range = mx[c] - mn[c];
                if(range > best_range) { best_range = range; best = i; best_ch = c; }
            }
        }
        if(best < 0 || best_range == 0) break;
        box_t * bx = &boxes[best];
        qsort(s + bx->from, (size_t)(bx->to - bx->from), sizeof(px_t), best_ch == 0 ? cmp_r : best_ch == 1 ? cmp_g : cmp_b);
        int mid = (bx->from + bx->to) / 2;
        boxes[nboxes].from = mid; boxes[nboxes].to = bx->to;
        bx->to = mid;
        nboxes++;
    }
    entry_t e[BOXES];
    for(int i = 0; i < nboxes; i++) {
        unsigned long r = 0, g = 0, b = 0;
        int cnt = boxes[i].to - boxes[i].from;
        for(int k = boxes[i].from; k < boxes[i].to; k++) { r += s[k].r; g += s[k].g; b += s[k].b; }
        e[i].count = cnt;
        e[i].rgb = cnt ? ((uint32_t)(r / (unsigned long)cnt) << 16) | ((uint32_t)(g / (unsigned long)cnt) << 8) | (uint32_t)(b / (unsigned long)cnt) : 0;
    }
    free(s);
    qsort(e, (size_t)nboxes, sizeof(entry_t), cmp_count_desc);

    /* the extremes among colours with a real share */
    int min_count = (int)(n * SHARE_MIN);
    if(min_count < 1) min_count = 1;
    double dom_hue = hue_of(e[0].rgb);
    double bl = -1, dl = 2, as = -1, a2 = -1;
    out->bright = out->dark = out->accent = out->accent2 = e[0].rgb;
    for(int i = 0; i < nboxes; i++) {
        if(e[i].count < min_count) continue;
        uint32_t c = e[i].rgb;
        double h, sa, l;
        to_hsl(c, &h, &sa, &l);
        if(l > bl) { bl = l; out->bright = c; }
        if(l < dl) { dl = l; out->dark = c; }
        double score = sa * (1 - fabs(l - 0.5));
        if(score > as) { as = score; out->accent = c; }
        double hd = fabs(h - dom_hue);
        if(hd > 180) hd = 360 - hd;
        double s2 = (sa >= 0.3 ? 1 : 0) * hd * sa;
        if(s2 > a2) { a2 = s2; out->accent2 = c; }
    }
    /* boxes that landed on the same colour (a big block of one colour is
     * split by the median cut into several boxes of it) are one colour with
     * their shares added: the share is what says "a big block" (0.17a) */
    for(int i = 0; i < nboxes; i++) {
        if(e[i].count == 0) continue;
        for(int j = i + 1; j < nboxes; j++) {
            if(e[j].count == 0 || dist(e[i].rgb, e[j].rgb) >= NEAR) continue;
            e[i].count += e[j].count;
            e[j].count = 0;
        }
    }
    int m = 0;
    for(int i = 0; i < nboxes; i++) if(e[i].count > 0) e[m++] = e[i];
    nboxes = m;
    qsort(e, (size_t)nboxes, sizeof(entry_t), cmp_count_desc);

    for(int i = 0; i < nboxes && i < PAL_DOM; i++) { out->dom[i] = e[i].rgb; out->share[i] = (uint8_t)((e[i].count * 100 + n / 2) / n); }
    out->ndom = (uint8_t)(nboxes < PAL_DOM ? nboxes : PAL_DOM);

    return true;
}

/* ----------------------------------------------------------- roles */

static bool reads(uint32_t bg, uint32_t c, double m) { return c != bg && palette_contrast(bg, c) >= m; }

static bool colourful(uint32_t c) { return chroma_of(c) >= 0.20 && light_of(c) >= 0.06 && light_of(c) <= 0.92; }

bool palette_roles(const art_palette_t * p, pal_mode_t mode, float min_contrast,
                   uint32_t cream, uint32_t dark_ink, uint32_t theme_accent, art_roles_t * out)
{
    (void)theme_accent;
    memset(out, 0, sizeof(*out));
    if(!p || p->ndom == 0) return false;
    double floor_c = min_contrast < 1 ? 1 : min_contrast;

    /* background */
    int max_col = 0, sum_col = 0;
    for(int i = 0; i < p->ndom; i++) if(colourful(p->dom[i])) { sum_col += p->share[i]; if(p->share[i] > max_col) max_col = p->share[i]; }
    bool only_colourful = max_col >= 12 || sum_col >= 25;
    uint32_t bg = p->dom[0];
    double best = -1;
    for(int i = 0; i < p->ndom; i++) {
        uint32_t c = p->dom[i];
        if(only_colourful && !colourful(c)) continue;
        bool ink_ok = (p->bright != c && palette_contrast(c, p->bright) >= floor_c) || (p->dark != c && palette_contrast(c, p->dark) >= floor_c);
        double score = p->share[i] / 100.0 + 0.4 * chroma_of(c) + (ink_ok ? 0.2 : 0) + 0.1 * light_of(c) - i * 0.01;
        if(score > best) { best = score; bg = c; }
    }
    if(mode == PAL_MODE_PAPER) {
        if(light_of(p->bright) >= 0.85) bg = p->bright;
        else if(light_of(p->dark) <= 0.12) bg = p->dark;
    }
    out->bg = bg;

    /* ink: the legible extreme */
    uint32_t ink = 0;
    bool have_ink = false;
    {
        uint32_t ex[2] = { p->bright, p->dark };
        if(palette_contrast(bg, ex[1]) > palette_contrast(bg, ex[0])) { uint32_t t = ex[0]; ex[0] = ex[1]; ex[1] = t; }
        for(int i = 0; i < 2 && !have_ink; i++) if(reads(bg, ex[i], 2.5)) { ink = ex[i]; have_ink = true; }
        if(!have_ink) ink = palette_contrast(bg, cream) >= palette_contrast(bg, dark_ink) ? cream : dark_ink;
    }
    out->ink = ink;
    out->sel = ink;

    /* text and play: the cover's colourful colours that read on bg, apart
     * from the ink and from each other */
    {
        uint32_t cc[PAL_DOM + 2];
        int cs[PAL_DOM + 2], nc = 0;
        uint32_t pool[PAL_DOM + 2];
        int ps[PAL_DOM + 2], np = 0;
        for(int i = 0; i < p->ndom; i++) { pool[np] = p->dom[i]; ps[np++] = p->share[i]; }
        pool[np] = p->accent; ps[np++] = 0;
        pool[np] = p->accent2; ps[np++] = 0;
        for(int i = 0; i < np; i++) {
            uint32_t c = pool[i];
            if(c == bg || dist(c, bg) < NEAR || !reads(bg, c, 2.5) || chroma_of(c) < 0.15 || dist(c, ink) < NEAR) continue;
            bool dup = false;
            for(int k = 0; k < nc; k++) if(dist(c, cc[k]) < 40) { dup = true; break; }
            if(dup) continue;
            cc[nc] = c; cs[nc] = ps[i]; nc++;
        }
        if(nc > 0) {
            int ti = 0;
            double bt = -1;
            for(int k = 0; k < nc; k++) { double sc = cs[k] / 100.0 + 0.5 * chroma_of(cc[k]); if(sc > bt) { bt = sc; ti = k; } }
            out->text = cc[ti];
            double bp = -1;
            for(int k = 0; k < nc; k++) {
                if(k == ti || dist(cc[k], out->text) < 100) continue;     /* clearly another colour, not a shade of the text's */
                if(chroma_of(cc[k]) < 0.4) continue;                       /* and a vivid one, or the theme lends one */
                if(chroma_of(cc[k]) > bp) { bp = chroma_of(cc[k]); out->play = cc[k]; }
            }
        }
    }
    return true;
}

/* ----------------------------------------------------------- index text */

void palette_format(const art_palette_t * p, char * out, size_t len)
{
    int k = 0;
    for(int i = 0; i < PAL_DOM; i++) k += snprintf(out + k, k < (int)len ? len - (size_t)k : 0, "%06x\t", i < p->ndom ? p->dom[i] : 0);
    k += snprintf(out + k, k < (int)len ? len - (size_t)k : 0, "%d\t%06x\t%06x\t%06x\t%06x", p->ndom, p->bright, p->dark, p->accent, p->accent2);
    for(int i = 0; i < PAL_DOM; i++) k += snprintf(out + k, k < (int)len ? len - (size_t)k : 0, "\t%d", i < p->ndom ? p->share[i] : 0);
}

bool palette_parse(const char * const * f, int n, art_palette_t * out)
{
    memset(out, 0, sizeof(*out));
    if(n < 13) return false;
    for(int i = 0; i < PAL_DOM; i++) out->dom[i] = (uint32_t)strtoul(f[i], NULL, 16);
    int nd = atoi(f[8]);
    out->ndom = (uint8_t)(nd < 0 ? 0 : nd > PAL_DOM ? PAL_DOM : nd);
    out->bright = (uint32_t)strtoul(f[9], NULL, 16);
    out->dark = (uint32_t)strtoul(f[10], NULL, 16);
    out->accent = (uint32_t)strtoul(f[11], NULL, 16);
    out->accent2 = (uint32_t)strtoul(f[12], NULL, 16);
    if(n >= PAL_FIELDS) for(int i = 0; i < PAL_DOM; i++) { int v = atoi(f[13 + i]); out->share[i] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); }
    return out->ndom > 0;
}
