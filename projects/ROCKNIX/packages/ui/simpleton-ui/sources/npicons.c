/*
 * SimpletonOS UI - the now-playing icons (implementation). See npicons.h.
 *
 * Shapes are lists of primitives in a unit square (0..1): filled polygons,
 * strokes (a polyline of a given width, round joins), and discs; a pixel's
 * alpha is how many of its 16 sample points fall inside any of them. The
 * geometry matches the mock-ups Ian approved (16 Oct 23:13).
 */
#include "npicons.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_SLOTS 32
#define SS 4                         /* samples per pixel per axis */
#define MAX_PTS 160

typedef struct {
    npicon_t kind;
    int size;
    uint32_t colour;
    uint8_t * px;
    lv_image_dsc_t dsc;
    unsigned age;
} slot_t;

static slot_t cache[CACHE_SLOTS];
static unsigned clock_now;

/* ---------------------------------------------------------- geometry */

typedef struct { float x, y; } pt_t;

typedef enum { SH_POLY, SH_STROKE, SH_DISC } shape_kind_t;

typedef struct {
    shape_kind_t kind;
    pt_t p[MAX_PTS];
    int n;
    float w;                     /* stroke width, or the disc's radius */
    bool closed;                 /* stroke: back to the first point */
    bool hole;                   /* subtracted from what came before */
} shape_t;

static bool in_poly(const shape_t * s, float x, float y)
{
    bool in = false;
    for(int i = 0, j = s->n - 1; i < s->n; j = i++) {
        const pt_t * a = &s->p[i], * b = &s->p[j];
        if(((a->y > y) != (b->y > y)) && (x < (b->x - a->x) * (y - a->y) / (b->y - a->y) + a->x)) in = !in;
    }
    return in;
}

static float seg_dist2(pt_t a, pt_t b, float x, float y)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float l2 = dx * dx + dy * dy;
    float t = l2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / l2 : 0;
    if(t < 0) t = 0; else if(t > 1) t = 1;
    float px = a.x + t * dx - x, py = a.y + t * dy - y;
    return px * px + py * py;
}

static bool in_stroke(const shape_t * s, float x, float y)
{
    float r2 = (s->w / 2) * (s->w / 2);
    int last = s->closed ? s->n : s->n - 1;
    for(int i = 0; i < last; i++) if(seg_dist2(s->p[i], s->p[(i + 1) % s->n], x, y) <= r2) return true;
    return false;
}

static bool in_shape(const shape_t * s, float x, float y)
{
    switch(s->kind) {
        case SH_POLY:   return in_poly(s, x, y);
        case SH_STROKE: return in_stroke(s, x, y);
        case SH_DISC:   { float dx = x - s->p[0].x, dy = y - s->p[0].y; return dx * dx + dy * dy <= s->w * s->w; }
    }
    return false;
}

/* --------------------------------------------------------- the shapes */

#define MAX_SHAPES 8

typedef struct { shape_t s[MAX_SHAPES]; int n; } icon_t;

static shape_t * add(icon_t * ic, shape_kind_t kind)
{
    if(ic->n >= MAX_SHAPES) return &ic->s[MAX_SHAPES - 1];
    shape_t * s = &ic->s[ic->n++];
    memset(s, 0, sizeof(*s));
    s->kind = kind;
    return s;
}

static void pt(shape_t * s, float x, float y) { if(s->n < MAX_PTS) { s->p[s->n].x = x; s->p[s->n].y = y; s->n++; } }

static void poly(icon_t * ic, int n, const float * xy)
{
    shape_t * s = add(ic, SH_POLY);
    for(int i = 0; i < n; i++) pt(s, xy[2 * i], xy[2 * i + 1]);
}

static shape_t * stroke(icon_t * ic, float w, int n, const float * xy)
{
    shape_t * s = add(ic, SH_STROKE);
    s->w = w;
    for(int i = 0; i < n; i++) pt(s, xy[2 * i], xy[2 * i + 1]);
    return s;
}

static void rect(icon_t * ic, float x0, float y0, float x1, float y1)
{
    float xy[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
    poly(ic, 4, xy);
}

/* the classic heart curve, scaled into the square */
static void heart_points(shape_t * s)
{
    for(int t = 0; t < 120; t++) {
        float a = (float)t / 120 * 2 * (float)M_PI;
        float sx = sinf(a);
        float x = 16 * sx * sx * sx;
        float y = 13 * cosf(a) - 5 * cosf(2 * a) - 2 * cosf(3 * a) - cosf(4 * a);
        pt(s, 0.5f + x / 36.0f, 0.5f - y / 36.0f + 0.05f);
    }
}

/* an arc of a circle as stroke points */
static void arc_points(shape_t * s, float cx, float cy, float r, float a0, float a1)
{
    int n = 40;
    for(int i = 0; i <= n; i++) {
        float a = a0 + (a1 - a0) * i / n;
        pt(s, cx + r * cosf(a), cy + r * sinf(a));
    }
}

static void arrowhead(icon_t * ic, float tipx, float tipy, float dirx, float diry, float len)
{
    /* an isosceles triangle pointing along (dirx, diry) */
    float nx = -diry, ny = dirx;
    float bx = tipx - dirx * len, by = tipy - diry * len;
    float xy[6] = { tipx, tipy, bx + nx * len * 0.62f, by + ny * len * 0.62f, bx - nx * len * 0.62f, by - ny * len * 0.62f };
    poly(ic, 3, xy);
}

static void build(npicon_t kind, icon_t * ic)
{
    memset(ic, 0, sizeof(*ic));
    const float W = 0.10f;              /* the stroke width of the line icons */
    switch(kind) {
        case NPI_PLAY: { float xy[6] = { 0.20f, 0.08f, 0.20f, 0.92f, 0.92f, 0.5f }; poly(ic, 3, xy); break; }
        case NPI_PAUSE: rect(ic, 0.20f, 0.10f, 0.42f, 0.90f); rect(ic, 0.58f, 0.10f, 0.80f, 0.90f); break;
        case NPI_NEXT: { float xy[6] = { 0.08f, 0.12f, 0.08f, 0.88f, 0.70f, 0.5f }; poly(ic, 3, xy); rect(ic, 0.74f, 0.12f, 0.90f, 0.88f); break; }
        case NPI_PREV: { float xy[6] = { 0.92f, 0.12f, 0.92f, 0.88f, 0.30f, 0.5f }; poly(ic, 3, xy); rect(ic, 0.10f, 0.12f, 0.26f, 0.88f); break; }
        case NPI_HEART_FILLED: { shape_t * s = add(ic, SH_POLY); heart_points(s); break; }
        case NPI_HEART: { shape_t * s = add(ic, SH_STROKE); s->w = W; s->closed = true; heart_points(s); break; }
        case NPI_LIST:
            for(int i = 0; i < 3; i++) { float y = 0.22f + i * 0.28f; float xy[4] = { 0.12f, y, 0.88f, y }; stroke(ic, W, 2, xy); }
            break;
        case NPI_GEAR: {
            shape_t * s = add(ic, SH_POLY);
            for(int i = 0; i < 16; i++) {
                float a = (float)i * (float)M_PI / 8;
                float r = (i % 2 == 0) ? 0.46f : 0.34f;
                pt(s, 0.5f + r * cosf(a), 0.5f + r * sinf(a));
            }
            shape_t * h = add(ic, SH_DISC); h->p[0].x = 0.5f; h->p[0].y = 0.5f; h->n = 1; h->w = 0.14f; h->hole = true;
            break;
        }
        case NPI_THROUGH: {
            float xy[4] = { 0.06f, 0.5f, 0.72f, 0.5f }; stroke(ic, W, 2, xy);
            arrowhead(ic, 0.96f, 0.5f, 1, 0, 0.30f);
            break;
        }
        case NPI_REPEAT:
        case NPI_REPEAT_ONE: {
            shape_t * a = add(ic, SH_STROKE); a->w = W; arc_points(a, 0.5f, 0.5f, 0.36f, (float)M_PI, 1.95f * (float)M_PI);
            shape_t * b = add(ic, SH_STROKE); b->w = W; arc_points(b, 0.5f, 0.5f, 0.36f, 0, 0.95f * (float)M_PI);
            arrowhead(ic, 0.86f, 0.60f, 0.0f, 1.0f, 0.24f);     /* the top arc's end, turning down */
            arrowhead(ic, 0.14f, 0.40f, 0.0f, -1.0f, 0.24f);    /* the bottom arc's end, turning up */
            if(kind == NPI_REPEAT_ONE) { float xy[6] = { 0.42f, 0.42f, 0.52f, 0.34f, 0.52f, 0.66f }; stroke(ic, 0.08f, 3, xy); }
            break;
        }
        case NPI_SHUFFLE: {
            float a[8] = { 0.04f, 0.26f, 0.30f, 0.26f, 0.62f, 0.74f, 0.76f, 0.74f }; stroke(ic, W, 4, a);
            float b[8] = { 0.04f, 0.74f, 0.30f, 0.74f, 0.62f, 0.26f, 0.76f, 0.26f }; stroke(ic, W, 4, b);
            arrowhead(ic, 0.98f, 0.74f, 1, 0, 0.24f);
            arrowhead(ic, 0.98f, 0.26f, 1, 0, 0.24f);
            break;
        }
        default: break;
    }
}

/* ------------------------------------------------------------ raster */

static void render(const icon_t * ic, int size, uint32_t colour, uint8_t * px)
{
    uint32_t stride = (uint32_t)size * 4;
    uint8_t r = (colour >> 16) & 0xFF, g = (colour >> 8) & 0xFF, b = colour & 0xFF;
    for(int y = 0; y < size; y++) {
        for(int x = 0; x < size; x++) {
            int hit = 0;
            for(int sy = 0; sy < SS; sy++) {
                for(int sx = 0; sx < SS; sx++) {
                    float fx = (x + (sx + 0.5f) / SS) / size, fy = (y + (sy + 0.5f) / SS) / size;
                    bool in = false;
                    for(int i = 0; i < ic->n; i++) {
                        const shape_t * s = &ic->s[i];
                        if(s->hole) { if(in && in_shape(s, fx, fy)) in = false; }
                        else if(!in && in_shape(s, fx, fy)) in = true;
                    }
                    if(in) hit++;
                }
            }
            uint8_t a = (uint8_t)(hit * 255 / (SS * SS));
            uint8_t * o = px + (size_t)y * stride + (size_t)x * 4;
            /* ARGB8888 as LVGL reads it on a little-endian target: B G R A, premultiplied is off */
            o[0] = b; o[1] = g; o[2] = r; o[3] = a;
        }
    }
}

const lv_image_dsc_t * npicon_dsc(npicon_t kind, int size, uint32_t colour)
{
    if(size < 4) size = 4;
    clock_now++;
    for(int i = 0; i < CACHE_SLOTS; i++) {
        if(cache[i].px && cache[i].kind == kind && cache[i].size == size && cache[i].colour == colour) { cache[i].age = clock_now; return &cache[i].dsc; }
    }
    int slot = -1;
    for(int i = 0; i < CACHE_SLOTS && slot < 0; i++) if(!cache[i].px) slot = i;
    if(slot < 0) { slot = 0; for(int i = 1; i < CACHE_SLOTS; i++) if(cache[i].age < cache[slot].age) slot = i; }
    uint32_t stride = (uint32_t)size * 4;
    uint8_t * px = malloc((size_t)stride * (size_t)size);
    if(!px) return NULL;
    static icon_t ic;                  /* ~8 KB: not on the stack */
    build(kind, &ic);
    render(&ic, size, colour, px);
    slot_t * c = &cache[slot];
    if(c->px) { lv_image_cache_drop(&c->dsc); free(c->px); }
    c->px = px; c->kind = kind; c->size = size; c->colour = colour; c->age = clock_now;
    memset(&c->dsc, 0, sizeof(c->dsc));
    c->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    c->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    c->dsc.header.w = (uint32_t)size;
    c->dsc.header.h = (uint32_t)size;
    c->dsc.header.stride = stride;
    c->dsc.data_size = stride * (uint32_t)size;
    c->dsc.data = px;
    return &c->dsc;
}
