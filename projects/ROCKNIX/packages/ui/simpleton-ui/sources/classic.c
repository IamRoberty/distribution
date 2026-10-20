/*
 * SimpletonOS UI - the classic visualizers (implementation). See classic.h.
 *
 * Spectrum: each bar is a clipped box whose height is the band's level,
 * standing on the baseline, holding a picture of the whole column (one
 * column image per look, drawn once: LED segments in the look's colour or
 * gradient, the gaps and the scanlines baked in, all opaque) aligned to its
 * bottom - so a bar at any height is the bottom of the same picture, as on
 * a real analyser, and a frame is copies only. The caps are plain boxes.
 *
 * Meters: two VU faces - an arc, ticks and numbers (LVGL objects, built
 * once), a needle (a two-point line) with the standard's ballistics (about
 * 300 ms to settle), the red zone past 0. Amp 95 and Amp 2000 have LED
 * ladders instead, as Winamp did.
 *
 * Scope: the left and right waveforms as lines across the grid's first
 * eight columns, and in the last four an XY plot (left against right,
 * turned so a mono sound stands up straight and stereo opens sideways).
 *
 * Spectrogram: time runs left to right, pitch up the screen on the same
 * log scale as Soundstage, loudness as colour on the look's own heat ramp;
 * one opaque picture scrolled a few pixels a frame.
 */
#include "classic.h"
#include "config.h"
#include "layout.h"
#include "palette.h"
#include "saver.h"
#include "strings.h"
#include "theme.h"
#include "tilestyle.h"
#include "viz.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS      33                   /* ~30 a second; the bars move on the audio thread's hops anyway */
#define SCOPE_MS      40
#define SPG_MS        66                   /* the spectrogram: a 6 px column every frame, ~20 s across a TV */
#define PEAK_HOLD_MS  450
#define PEAK_FALL     1.1f                 /* of the full height a second */
#define BAR_RISE      0.55f                /* the share of the way to a louder value each frame */
#define BAR_FALL      0.22f
#define VU_ALPHA      0.40f                /* a frame's share of the way: ~300 ms to settle, the VU standard */
#define SCOPE_N       320                  /* points across each waveform */
#define XY_N          400
#define LADDER_N      24

typedef struct {
    const char * code;
    int          name_id;
    uint32_t     ground, bar, cap, keyline;      /* keyline 0 = none */
    const uint32_t * grad; int ngrad;            /* the segments' colours up the height; NULL = the bar colour throughout */
    bool         scanlines;
    bool         dealt;                          /* archer / record: the colours come from the deal or the album */
    uint32_t     face, faceink, needle, hot;     /* meters: a face (0 = none, drawn on the ground), its ink, the needle, past 0 */
    bool         ladder;                         /* meters: LED ladders, not needles */
    const uint32_t * spg; int nspg;              /* the spectrogram's ramp, quiet to loud; NULL = ground -> bar -> cap */
} look_t;

static const uint32_t amp95_grad[]   = { 0x189C18, 0x7FC418, 0xCECE18, 0xE08A18, 0xDE2810 };
static const uint32_t amp2000_grad[] = { 0x4DDFF9, 0x44B5F4, 0x3B93F0, 0x5171EC, 0x7660F3, 0x9456F0, 0xC251EE, 0xF2A2F8 };
static const uint32_t amp95_spg[]    = { 0x000000, 0x0A3A0A, 0x189C18, 0xCECE18, 0xDE2810 };
static const uint32_t phosphor_spg[] = { 0x040A06, 0x0B3A1C, 0x1E9A4C, 0x39FF7A, 0xD8FFE6 };
static const uint32_t amp2000_spg[]  = { 0x0B0F1E, 0x24346E, 0x3B93F0, 0x4DDFF9, 0x9456F0, 0xF2A2F8 };
static const uint32_t ice_spg[]      = { 0x151C2B, 0x24346E, 0x2E5FBC, 0x44B5F4, 0x6BE9F7, 0xE6E5F4 };
static const uint32_t uv_spg[]       = { 0x140C26, 0x2D1A50, 0x493A89, 0x764DC9, 0x9456F0, 0xD77AF6, 0xF2A2F8 };
static const uint32_t black_spg[]    = { 0x1E3A80, 0x2C50A4, 0x7660F3, 0xC251EE, 0x94EDF8 };
static const uint32_t steam_spg[]    = { 0x120C08, 0x2A170C, 0x5C2E14, 0x8A4B22, 0xB87333, 0xD4A54A, 0xF0DFB0 };

static const look_t looks[] = {
    { "archer",     S_LOOK_ARCHER,     0x35274A, 0xEABE94, 0xE1BD6D, 0xFAEFD0, NULL, 0,         false, true,  0xEABE94, 0x35274A, 0x35274A, 0x0B775E, false, NULL, 0 },
    { "record",     S_LOOK_RECORD,     0x1A1612, 0xC0A080, 0xFAEFD0, 0xFAEFD0, NULL, 0,         false, true,  0xFAEFD0, 0x1A1612, 0x1A1612, 0xC0A080, false, NULL, 0 },
    { "amp95",      S_LOOK_AMP95,      0x000000, 0x189C18, 0xA0A0A0, 0,        amp95_grad, 5,   false, false, 0,        0x5AB45A, 0x5AB45A, 0xDE2810, true,  amp95_spg, 5 },
    { "phosphor",   S_LOOK_PHOSPHOR,   0x040A06, 0x39FF7A, 0xB8FFD0, 0,        NULL, 0,         false, false, 0,        0x39FF7A, 0x39FF7A, 0x39FF7A, false, phosphor_spg, 5 },
    { "amp2000",    S_LOOK_AMP2000,    0x0B0F1E, 0x4DDFF9, 0xC3F1FA, 0,        amp2000_grad, 8, true,  false, 0,        0x4DDFF9, 0xF2A2F8, 0xF2A2F8, true,  amp2000_spg, 6 },
    { "ice",        S_LOOK_ICE,        0x151C2B, 0x6BE9F7, 0xE6E5F4, 0,        NULL, 0,         true,  false, 0,        0x6BE9F7, 0xC3F1FA, 0xC3F1FA, false, ice_spg, 6 },
    { "uv",         S_LOOK_UV,         0x140C26, 0x9456F0, 0xF2A2F8, 0,        NULL, 0,         true,  false, 0,        0xA282EE, 0xD77AF6, 0xF2A2F8, false, uv_spg, 7 },
    { "blacklight", S_LOOK_BLACKLIGHT, 0x2C50A4, 0xC251EE, 0x94EDF8, 0x94EDF8, NULL, 0,         true,  false, 0x2E5FBC, 0xE6E5F4, 0x94EDF8, 0xF2A2F8, false, black_spg, 5 },
    { "steam",      S_LOOK_STEAM,      0x120C08, 0xB87333, 0xF0DFB0, 0xC9A04A, NULL, 0,         false, false, 0xE8D3A2, 0x3B2314, 0x3B2314, 0x8A2E1A, false, steam_spg, 7 },
};
#define NLOOKS ((int)(sizeof(looks) / sizeof(looks[0])))

int          classic_look_count(void)      { return NLOOKS; }
const char * classic_look_code(int i)      { return i >= 0 && i < NLOOKS ? looks[i].code : ""; }
int          classic_look_name_id(int i)   { return i >= 0 && i < NLOOKS ? looks[i].name_id : S_LOOK_ARCHER; }

static lv_obj_t * root;
static lv_timer_t * timer;
static classic_kind_t kind;
static look_t cur;                          /* the look in use, colours filled */
static char track_file[1024];
static uint32_t last_track_ms;
static int W, H;                            /* the screen */

static uint32_t mixc(uint32_t a, uint32_t b, float t)
{
    int r = (int)(((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t);
    int g = (int)(((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t);
    int bl = (int)((a & 255) * (1 - t) + (b & 255) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static uint32_t grad_at(const uint32_t * g, int n, float t)
{
    if(n < 2) return g[0];
    float x = (t < 0 ? 0 : t > 1 ? 1 : t) * (n - 1); int i = (int)x; if(i >= n - 1) i = n - 2;
    return mixc(g[i], g[i + 1], x - i);
}

static uint32_t dim(uint32_t c) { return mixc(c, 0x000000, 0.40f); }
static bool scan_row(int screen_y) { return cur.scanlines && screen_y % 3 == 0; }
static int px(int v1080) { return (v1080 * H + 540) / 1080; }        /* a size at 1080p, scaled to this screen's height */

static void image_dsc(lv_image_dsc_t * d, uint8_t * buf, int w, int h)
{
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_XRGB8888;
    d->header.w = (uint32_t)w; d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 4;
    d->data_size = (uint32_t)w * (uint32_t)h * 4;
    d->data = buf;
}

/* ------------------------------------------------------------ the colours */

/* archer: a tile colourway dealt from the theme; the ground its colour set
 * back, the bars its colour, the caps its ink; a face in its colour */
static void deal_colours(void)
{
    tile_style_t st = { 0 };
    bool noart = true;
    tilestyle_deal((unsigned)lv_tick_get() ^ 0x9e3779b9u, 1, 1, &noart, NULL, &st);
    if(!st.bg) { st.bg = 0xEABE94; st.ink = 0x35274A; }
    cur.ground = ui_mix(st.bg, 0x000000, 70);                     /* the colour, well back: the bars come forward in it */
    cur.bar = st.bg;
    cur.cap = st.ink;
    cur.keyline = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0;
    if(palette_contrast(cur.ground, cur.cap) < 2.0) cur.cap = cur.keyline;
    cur.face = st.bg; cur.faceink = st.ink; cur.needle = st.ink; cur.hot = tilestyle_accent() ? tilestyle_accent() : st.ink;
    if(palette_contrast(cur.face, cur.hot) < 2.0) cur.hot = st.ink;
}

static void apply_colours(void);

/* record: the playing album's colours, re-read when the track changes */
static void record_colours(bool force)
{
    uint32_t g, b, c; char file[1024];
    if(!saver_playing_colours(&g, &b, &c, file, sizeof(file))) { if(force) deal_colours(); return; }
    if(!force && strcmp(file, track_file) == 0) return;
    snprintf(track_file, sizeof(track_file), "%s", file);
    cur.ground = g; cur.bar = b; cur.cap = c; cur.keyline = c;
    cur.face = c; cur.faceink = g; cur.needle = g; cur.hot = b;
    if(palette_contrast(cur.face, cur.hot) < 2.0) cur.hot = g;
    apply_colours();
}

static void set_look(const char * look)
{
    int li = 0;
    for(int i = 0; i < NLOOKS; i++) if(look && strcmp(looks[i].code, look) == 0) li = i;
    cur = looks[li];
    track_file[0] = 0;
    if(strcmp(cur.code, "archer") == 0) deal_colours();
    else if(strcmp(cur.code, "record") == 0) record_colours(true);
}

/* ------------------------------------------------------------ Spectrum */

static lv_obj_t * sp_root;
static lv_obj_t * bars[VIZ_BANDS], * caps[VIZ_BANDS];
static lv_image_dsc_t col_dsc, ground_dsc;
static uint8_t * col_buf, * ground_buf;
static int bw, gap, sp_top, sp_bottom, hr;
static float level[VIZ_BANDS], peak[VIZ_BANDS];
static uint32_t peak_at[VIZ_BANDS];

static void make_column(void)
{
    free(col_buf);
    col_buf = calloc((size_t)bw * (size_t)hr, 4);
    if(!col_buf) return;
    int seg = hr / 54 > 6 ? hr / 54 : 6, gap_px = seg / 4 > 2 ? seg / 4 : 2, step = seg + gap_px;
    for(int y = 0; y < hr; y++) {
        int from_bottom = hr - 1 - y;
        bool lit = (from_bottom % step) < seg;                    /* every look in LED segments (Ian, 19 Oct 22:59) */
        uint32_t c = lit ? (cur.grad ? grad_at(cur.grad, cur.ngrad, (float)from_bottom / hr) : cur.bar) : cur.ground;
        if(scan_row(sp_top + y)) c = dim(c);
        uint32_t pxv = 0xFF000000u | c;
        uint32_t * row = (uint32_t *)(col_buf + (size_t)y * bw * 4);
        for(int x = 0; x < bw; x++) row[x] = pxv;
    }
    image_dsc(&col_dsc, col_buf, bw, hr);
}

/* the ground with scanlines: an opaque tile the screen's width and three
 * rows tall (the first dark), tiled down the root from its top */
static void make_ground(void)
{
    if(!ground_buf) ground_buf = calloc((size_t)W * 3, 4);
    if(!ground_buf) return;
    uint32_t * p = (uint32_t *)ground_buf;
    for(int y = 0; y < 3; y++) for(int x = 0; x < W; x++) p[y * W + x] = 0xFF000000u | (y == 0 ? dim(cur.ground) : cur.ground);
    image_dsc(&ground_dsc, ground_buf, W, 3);
}

static void spectrum_create(void)
{
    sp_root = lv_obj_create(root);
    lv_obj_remove_style_all(sp_root);
    lv_obj_set_size(sp_root, W, H);
    lv_obj_remove_flag(sp_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sp_root, LV_OBJ_FLAG_HIDDEN);
    int pitch = W / VIZ_BANDS;
    gap = pitch / 5 > 2 ? pitch / 5 : 2;
    bw = pitch - gap;
    int x0 = (W - pitch * VIZ_BANDS + gap) / 2;
    int margin = ui_screen.kind == UI_SCREEN_TV ? H * 16 / 100 : H * 10 / 100;
    sp_top = margin; sp_bottom = H - margin; hr = sp_bottom - sp_top;
    for(int i = 0; i < VIZ_BANDS; i++) {
        lv_obj_t * b = lv_obj_create(sp_root);
        lv_obj_remove_style_all(b);
        lv_obj_set_pos(b, x0 + i * pitch, sp_bottom - 2);
        lv_obj_set_size(b, bw, 2);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t * img = lv_image_create(b);
        lv_obj_set_size(img, bw, hr);
        lv_obj_align(img, LV_ALIGN_BOTTOM_MID, 0, 0);
        bars[i] = b;
        lv_obj_t * c = lv_obj_create(sp_root);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, bw, gap > 4 ? gap : 4);
        lv_obj_set_pos(c, x0 + i * pitch, sp_bottom - gap * 3);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        caps[i] = c;
    }
}

static void spectrum_colours(void)
{
    make_column();
    for(int i = 0; i < VIZ_BANDS; i++) {
        lv_obj_t * img = lv_obj_get_child(bars[i], 0);
        if(img) { lv_image_set_src(img, NULL); lv_image_set_src(img, &col_dsc); }
        lv_obj_set_style_bg_color(caps[i], lv_color_hex(cur.cap), 0);
    }
}

static void spectrum_frame(uint32_t now)
{
    float v[VIZ_BANDS];
    if(!viz_bands(v, VIZ_BANDS)) memset(v, 0, sizeof(v));
    for(int i = 0; i < VIZ_BANDS; i++) {
        float target = v[i];
        level[i] += (target - level[i]) * (target > level[i] ? BAR_RISE : BAR_FALL);
        if(level[i] >= peak[i]) { peak[i] = level[i]; peak_at[i] = now; }
        else if(now - peak_at[i] > PEAK_HOLD_MS) { peak[i] -= PEAK_FALL * FRAME_MS / 1000.0f; if(peak[i] < level[i]) peak[i] = level[i]; }
        int h = (int)(level[i] * hr); if(h < 2) h = 2;
        lv_obj_set_y(bars[i], sp_bottom - h);
        lv_obj_set_height(bars[i], h);
        int ph = (int)(peak[i] * hr);
        lv_obj_set_y(caps[i], sp_bottom - ph - gap * 2 - gap / 2);
    }
}

static void spectrum_reset(void)
{
    memset(level, 0, sizeof(level)); memset(peak, 0, sizeof(peak)); memset(peak_at, 0, sizeof(peak_at));
    for(int i = 0; i < VIZ_BANDS; i++) { lv_obj_set_y(bars[i], sp_bottom - 2); lv_obj_set_height(bars[i], 2); lv_obj_set_y(caps[i], sp_bottom - gap * 3); }
}

/* ------------------------------------------------------------ Meters */

#define TICKS 11
static const int tick_v[TICKS] = { -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 };

typedef struct {
    int cx, cy, r;
    lv_obj_t * face, * keyline, * arc, * arc_hot, * ticks[TICKS], * nums[6], * vu, * name, * needle, * pivot;
    lv_point_precise_t needle_pts[2], tick_pts[TICKS][2];
    lv_obj_t * ladder[LADDER_N], * ladder_nums[5];
    float db;
} meter_t;
static lv_obj_t * mt_root;
static meter_t mt[2];

/* the needle's angle for a VU reading: -50 degrees at -20, +50 at +3, the
 * scale stretched like a real meter's (the top end has the room) */
static float vu_angle(float v) { float t = (v + 20) / 23.0f; if(t < 0) t = 0; if(t > 1) t = 1; return -50 + 100 * powf(t, 0.75f); }

static void meter_build(meter_t * m, int cx, int cy, int r, int label_id)
{
    m->cx = cx; m->cy = cy; m->r = r;
    int fw = 2 * r + px(120), fh = r + px(220);
    m->face = lv_obj_create(mt_root);
    lv_obj_remove_style_all(m->face);
    lv_obj_set_size(m->face, fw, fh);
    lv_obj_set_pos(m->face, cx - fw / 2, cy - r - px(100));
    lv_obj_set_style_bg_opa(m->face, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m->face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    m->keyline = ui_sign(m->face);
    /* the arc: from -50 to +50 degrees about straight up (LVGL's angles run from 3 o'clock, clockwise) */
    m->arc = lv_arc_create(mt_root);
    lv_obj_remove_style_all(m->arc);
    lv_obj_remove_style(m->arc, NULL, LV_PART_KNOB);
    lv_obj_set_size(m->arc, 2 * r, 2 * r);
    lv_obj_set_pos(m->arc, cx - r, cy - r);
    lv_arc_set_bg_angles(m->arc, 220, 320);
    lv_arc_set_angles(m->arc, 220, 220);
    lv_obj_set_style_arc_width(m->arc, px(4), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(m->arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(m->arc, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(m->arc, LV_OBJ_FLAG_CLICKABLE);
    m->arc_hot = lv_arc_create(mt_root);
    lv_obj_remove_style_all(m->arc_hot);
    lv_obj_remove_style(m->arc_hot, NULL, LV_PART_KNOB);
    lv_obj_set_size(m->arc_hot, 2 * r, 2 * r);
    lv_obj_set_pos(m->arc_hot, cx - r, cy - r);
    lv_arc_set_bg_angles(m->arc_hot, (int)(270 + vu_angle(0)), (int)(270 + vu_angle(3)));
    lv_arc_set_angles(m->arc_hot, 220, 220);
    lv_obj_set_style_arc_width(m->arc_hot, px(12), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(m->arc_hot, false, LV_PART_MAIN);
    lv_obj_set_style_arc_width(m->arc_hot, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(m->arc_hot, LV_OBJ_FLAG_CLICKABLE);
    int ni = 0;
    for(int i = 0; i < TICKS; i++) {
        float a = vu_angle((float)tick_v[i]) * (float)M_PI / 180;
        bool big = tick_v[i] == -20 || tick_v[i] == -10 || tick_v[i] == -5 || tick_v[i] == 0 || tick_v[i] == 3;
        int r0 = r - (big ? px(34) : px(22)), r1 = r;
        m->tick_pts[i][0].x = cx + r0 * sinf(a); m->tick_pts[i][0].y = cy - r0 * cosf(a);
        m->tick_pts[i][1].x = cx + r1 * sinf(a); m->tick_pts[i][1].y = cy - r1 * cosf(a);
        m->ticks[i] = lv_line_create(mt_root);
        lv_line_set_points(m->ticks[i], m->tick_pts[i], 2);
        lv_obj_set_style_line_width(m->ticks[i], px(5), 0);
        if(tick_v[i] == -20 || tick_v[i] == -10 || tick_v[i] == -5 || tick_v[i] == -3 || tick_v[i] == 0 || tick_v[i] == 3) {
            char t[8]; snprintf(t, sizeof(t), "%s%d", tick_v[i] > 0 ? "+" : "", tick_v[i]);
            lv_obj_t * l = lv_label_create(mt_root);
            lv_label_set_text(l, t);
            lv_obj_set_style_text_font(l, ui_font_px(px(30)), 0);
            lv_obj_update_layout(l);
            int rt = r + px(40);
            lv_obj_set_pos(l, (int)(cx + rt * sinf(a)) - lv_obj_get_width(l) / 2, (int)(cy - rt * cosf(a)) - lv_obj_get_height(l) / 2);
            m->nums[ni++] = l;
        }
    }
    m->vu = lv_label_create(mt_root);
    lv_label_set_text(m->vu, "VU");
    lv_obj_set_style_text_font(m->vu, ui_font_face_px("Bevan-Regular.ttf", px(54), false), 0);
    lv_obj_update_layout(m->vu);
    lv_obj_set_pos(m->vu, cx - lv_obj_get_width(m->vu) / 2, cy - r * 42 / 100 - lv_obj_get_height(m->vu) / 2);
    m->name = lv_label_create(mt_root);
    { char caps[64]; ui_caps(T((str_id_t)label_id), caps, sizeof(caps)); lv_label_set_text(m->name, caps); }
    lv_obj_set_style_text_font(m->name, ui_font_caps(px(30)), 0);
    lv_obj_update_layout(m->name);
    lv_obj_set_pos(m->name, cx - lv_obj_get_width(m->name) / 2, cy + px(70) - lv_obj_get_height(m->name) / 2);
    m->needle = lv_line_create(mt_root);
    lv_obj_set_style_line_width(m->needle, px(6), 0);
    lv_obj_set_style_line_rounded(m->needle, true, 0);
    m->pivot = lv_obj_create(mt_root);
    lv_obj_remove_style_all(m->pivot);
    lv_obj_set_size(m->pivot, px(44), px(44));
    lv_obj_set_pos(m->pivot, cx - px(22), cy + px(20) - px(22));
    lv_obj_set_style_radius(m->pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(m->pivot, LV_OPA_COVER, 0);
    /* the ladder (Amp 95 / Amp 2000): across the face, where the arc would be */
    int lw = 2 * r + px(80), sw = lw / LADDER_N;
    for(int k = 0; k < LADDER_N; k++) {
        lv_obj_t * s = lv_obj_create(mt_root);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, sw - px(6), px(140));
        lv_obj_set_pos(s, cx - lw / 2 + k * sw + px(3), cy - px(120));
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        m->ladder[k] = s;
    }
    static const int lv_at[5] = { -20, -10, -5, 0, 3 };
    static const char * const lv_txt[5] = { "-20", "-10", "-5", "0", "+3" };
    for(int k = 0; k < 5; k++) {
        lv_obj_t * l = lv_label_create(mt_root);
        lv_label_set_text(l, lv_txt[k]);
        lv_obj_set_style_text_font(l, ui_font_px(px(26)), 0);
        lv_obj_update_layout(l);
        int x = cx - lw / 2 + (int)((lv_at[k] + 20) / 23.0f * lw);
        lv_obj_set_pos(l, x - lv_obj_get_width(l) / 2, cy - px(170) - lv_obj_get_height(l) / 2);
        m->ladder_nums[k] = l;
    }
    m->db = -20;
}

static void meter_colours(meter_t * m)
{
    bool ladder = cur.ladder;
    bool face = cur.face != 0 && !ladder;
    if(face) { lv_obj_set_style_bg_color(m->face, lv_color_hex(cur.face), 0); lv_obj_remove_flag(m->face, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(m->face, LV_OBJ_FLAG_HIDDEN);
    if(face && cur.keyline) { ui_sign_colour(m->keyline, cur.faceink); lv_obj_remove_flag(m->keyline, LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(m->keyline, LV_OBJ_FLAG_HIDDEN);
    uint32_t ink = cur.faceink;
    lv_obj_set_style_arc_color(m->arc, lv_color_hex(ink), LV_PART_MAIN);
    lv_obj_set_style_arc_color(m->arc_hot, lv_color_hex(cur.hot), LV_PART_MAIN);
    for(int i = 0; i < TICKS; i++) lv_obj_set_style_line_color(m->ticks[i], lv_color_hex(tick_v[i] > 0 ? cur.hot : ink), 0);
    for(int i = 0; i < 6; i++) lv_obj_set_style_text_color(m->nums[i], lv_color_hex(i == 5 ? cur.hot : ink), 0);
    lv_obj_set_style_text_color(m->vu, lv_color_hex(ink), 0);
    lv_obj_set_style_text_color(m->name, lv_color_hex(ink), 0);
    lv_obj_set_style_line_color(m->needle, lv_color_hex(cur.needle), 0);
    lv_obj_set_style_bg_color(m->pivot, lv_color_hex(cur.needle), 0);
    lv_obj_t * needle_parts[] = { m->arc, m->arc_hot, m->vu, m->needle, m->pivot };
    for(int i = 0; i < 5; i++) { if(ladder) lv_obj_add_flag(needle_parts[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(needle_parts[i], LV_OBJ_FLAG_HIDDEN); }
    for(int i = 0; i < TICKS; i++) { if(ladder) lv_obj_add_flag(m->ticks[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(m->ticks[i], LV_OBJ_FLAG_HIDDEN); }
    for(int i = 0; i < 6; i++) { if(ladder) lv_obj_add_flag(m->nums[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(m->nums[i], LV_OBJ_FLAG_HIDDEN); }
    for(int k = 0; k < LADDER_N; k++) { if(ladder) lv_obj_remove_flag(m->ladder[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(m->ladder[k], LV_OBJ_FLAG_HIDDEN); }
    for(int k = 0; k < 5; k++) { lv_obj_set_style_text_color(m->ladder_nums[k], lv_color_hex(ink), 0); if(ladder) lv_obj_remove_flag(m->ladder_nums[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(m->ladder_nums[k], LV_OBJ_FLAG_HIDDEN); }
    if(ladder) { lv_obj_set_pos(m->name, m->cx - lv_obj_get_width(m->name) / 2, m->cy + px(50)); }
    else lv_obj_set_pos(m->name, m->cx - lv_obj_get_width(m->name) / 2, m->cy + px(70) - lv_obj_get_height(m->name) / 2);
}

static void meter_show(meter_t * m, float rms)
{
    /* 0 VU at -9 dBFS RMS (0.33a): the broadcast alignment (-18) pinned the
     * needles on modern records (Ian, 20 Oct); at -9 a loud master swings
     * round 0 and a quiet one sits in the middle of the scale */
    float db = rms > 1e-5f ? 20 * log10f(rms) + 9 : -40;
    db = db < -20 ? -20 : db > 3 ? 3 : db;
    m->db += (db - m->db) * VU_ALPHA;
    if(cur.ladder) {
        float lit = (m->db + 20) / 23.0f * LADDER_N;
        for(int k = 0; k < LADDER_N; k++) {
            uint32_t c = grad_at(cur.grad, cur.ngrad, (float)k / (LADDER_N - 1));
            lv_obj_set_style_bg_color(m->ladder[k], lv_color_hex(k < lit ? c : mixc(c, cur.ground, 0.82f)), 0);
        }
        return;
    }
    float a = vu_angle(m->db) * (float)M_PI / 180; int L = m->r + px(14);
    m->needle_pts[0].x = m->cx; m->needle_pts[0].y = m->cy + px(20);
    m->needle_pts[1].x = m->cx + L * sinf(a); m->needle_pts[1].y = m->cy - L * cosf(a);
    lv_line_set_points(m->needle, m->needle_pts, 2);
}

static void meters_create(void)
{
    mt_root = lv_obj_create(root);
    lv_obj_remove_style_all(mt_root);
    lv_obj_set_size(mt_root, W, H);
    lv_obj_remove_flag(mt_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(mt_root, LV_OBJ_FLAG_HIDDEN);
    if(ui_screen.kind == UI_SCREEN_TV || W > H * 3 / 2) {
        int r = H * 33 / 100;
        meter_build(&mt[0], W / 4, H * 60 / 100, r, S_METER_LEFT);
        meter_build(&mt[1], W * 3 / 4, H * 60 / 100, r, S_METER_RIGHT);
    }
    else {                                                        /* the handheld: one above the other */
        int r = W * 24 / 100;
        meter_build(&mt[0], W / 2, H * 30 / 100, r, S_METER_LEFT);
        meter_build(&mt[1], W / 2, H * 78 / 100, r, S_METER_RIGHT);
    }
}

static void meters_frame(void)
{
    float l, r;
    viz_levels(&l, &r);
    meter_show(&mt[0], l);
    meter_show(&mt[1], r);
}

/* ------------------------------------------------------------ Scope */

static lv_obj_t * sc_root, * wave[2], * wave_lab[2], * xy, * xy_box, * xy_cross[2];
static lv_point_precise_t wave_pts[2][SCOPE_N], xy_pts[XY_N], cross_pts[2][2];
static int sc_x0, sc_x1, sc_yc[2], sc_amp, xy_cx, xy_cy, xy_side;
static float sc_gain = 1;                   /* a slow auto gain: quiet music still draws a wave (up to 6x) */

static void scope_create(void)
{
    sc_root = lv_obj_create(root);
    lv_obj_remove_style_all(sc_root);
    lv_obj_set_size(sc_root, W, H);
    lv_obj_remove_flag(sc_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(sc_root, LV_OBJ_FLAG_HIDDEN);
    bool wide = ui_screen.kind == UI_SCREEN_TV || W > H * 3 / 2;
    if(wide) {
        sc_x0 = W * 4 / 100; sc_x1 = W * 62 / 100;
        sc_yc[0] = H * 30 / 100; sc_yc[1] = H * 70 / 100; sc_amp = H * 14 / 100;
        xy_side = H * 50 / 100; xy_cx = W * 82 / 100; xy_cy = H / 2;
    }
    else {                                                        /* the handheld: the waves above, the XY below */
        sc_x0 = W * 5 / 100; sc_x1 = W * 95 / 100;
        sc_yc[0] = H * 16 / 100; sc_yc[1] = H * 40 / 100; sc_amp = H * 9 / 100;
        xy_side = H * 36 / 100; xy_cx = W / 2; xy_cy = H * 76 / 100;
    }
    for(int c = 0; c < 2; c++) {
        wave[c] = lv_line_create(sc_root);
        lv_obj_set_style_line_width(wave[c], px(4), 0);
        lv_obj_set_style_line_rounded(wave[c], true, 0);
        for(int i = 0; i < SCOPE_N; i++) { wave_pts[c][i].x = sc_x0 + (sc_x1 - sc_x0) * i / (SCOPE_N - 1); wave_pts[c][i].y = sc_yc[c]; }
        lv_line_set_points(wave[c], wave_pts[c], SCOPE_N);
        wave_lab[c] = lv_label_create(sc_root);
        lv_label_set_text(wave_lab[c], c ? "R" : "L");
        lv_obj_set_style_text_font(wave_lab[c], ui_font_caps(px(30)), 0);
        lv_obj_set_pos(wave_lab[c], sc_x0 - px(44), sc_yc[c] - px(18));
    }
    xy_box = lv_obj_create(sc_root);
    lv_obj_remove_style_all(xy_box);
    lv_obj_set_size(xy_box, xy_side, xy_side);
    lv_obj_set_pos(xy_box, xy_cx - xy_side / 2, xy_cy - xy_side / 2);
    lv_obj_set_style_border_width(xy_box, px(2), 0);
    lv_obj_set_style_border_opa(xy_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(xy_box, 0, 0);
    for(int d = 0; d < 2; d++) {
        cross_pts[d][0].x = xy_cx - xy_side / 2; cross_pts[d][0].y = d ? xy_cy + xy_side / 2 : xy_cy - xy_side / 2;
        cross_pts[d][1].x = xy_cx + xy_side / 2; cross_pts[d][1].y = d ? xy_cy - xy_side / 2 : xy_cy + xy_side / 2;
        xy_cross[d] = lv_line_create(sc_root);
        lv_line_set_points(xy_cross[d], cross_pts[d], 2);
        lv_obj_set_style_line_width(xy_cross[d], 1, 0);
    }
    xy = lv_line_create(sc_root);
    lv_obj_set_style_line_width(xy, px(3), 0);
    for(int i = 0; i < XY_N; i++) { xy_pts[i].x = xy_cx; xy_pts[i].y = xy_cy; }
    lv_line_set_points(xy, xy_pts, XY_N);
}

static void scope_colours(void)
{
    for(int c = 0; c < 2; c++) { lv_obj_set_style_line_color(wave[c], lv_color_hex(cur.bar), 0); lv_obj_set_style_text_color(wave_lab[c], lv_color_hex(cur.faceink), 0); }
    uint32_t grid = mixc(cur.ground, cur.faceink, 0.45f);
    lv_obj_set_style_border_color(xy_box, lv_color_hex(grid), 0);
    for(int d = 0; d < 2; d++) lv_obj_set_style_line_color(xy_cross[d], lv_color_hex(grid), 0);
    lv_obj_set_style_line_color(xy, lv_color_hex(cur.cap), 0);
}

static void scope_frame(void)
{
    static int16_t l[2048], r[2048];
    int n = viz_wave(l, r, 2048);
    if(!n) { memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); n = 2048; }
    /* the gain: the loudest sample of this frame, tracked slowly, fills the trace */
    int pk = 1; for(int i = 0; i < n; i++) { int a = abs(l[i]), b = abs(r[i]); if(a > pk) pk = a; if(b > pk) pk = b; }
    float want = 28000.0f / pk; if(want > 6) want = 6; if(want < 1) want = 1;
    sc_gain += (want - sc_gain) * (want < sc_gain ? 0.5f : 0.02f);      /* down at once, up slowly */
    /* the waves: 2048 samples (46 ms) across, every sixth or so */
    for(int c = 0; c < 2; c++) {
        const int16_t * s = c ? r : l;
        for(int i = 0; i < SCOPE_N; i++) {
            int k = (int)((long)i * (n - 1) / (SCOPE_N - 1));
            float v = s[k] * sc_gain / 32768; if(v > 1) v = 1; if(v < -1) v = -1;
            wave_pts[c][i].y = sc_yc[c] - (int)(v * sc_amp);
        }
        lv_line_set_points(wave[c], wave_pts[c], SCOPE_N);
    }
    /* the XY: the newest 400 samples, left against right, turned 45 degrees */
    float sc = xy_side * 0.40f / 32768 * sc_gain;
    for(int i = 0; i < XY_N; i++) {
        int k = n - XY_N + i; if(k < 0) k = 0;
        float m = (l[k] + r[k]) * 0.5f, s = (l[k] - r[k]) * 0.5f;
        float xx = s * sc * 1.4f, yy = m * sc; float lim = xy_side * 0.47f;
        xx = xx > lim ? lim : xx < -lim ? -lim : xx;
        yy = yy > lim ? lim : yy < -lim ? -lim : yy;
        xy_pts[i].x = xy_cx + (int)xx; xy_pts[i].y = xy_cy - (int)yy;
    }
    lv_line_set_points(xy, xy_pts, XY_N);
}

/* ------------------------------------------------------------ Spectrogram */

static lv_obj_t * spg_root, * spg_img;
static lv_image_dsc_t spg_dsc;
static uint8_t * spg_buf;
static int spg_w, spg_h, spg_x, spg_y, spg_step;
static uint32_t spg_lut[256];

/* which way it flows (Ian, 20 Oct): the newest moment enters at one edge
 * and the picture moves away from it. rtl: in at the right, moving left
 * (the first one); ltr: in at the left; ttb: in at the top, moving down;
 * btt: in at the bottom, moving up. Across the flow, pitch runs low to high:
 * bottom to top for rtl/ltr, left to right for ttb/btt. */
typedef enum { SPG_RTL = 0, SPG_LTR, SPG_TTB, SPG_BTT } spg_dir_t;
static spg_dir_t spg_dir;
static const char * const spg_dir_codes[4] = { "rtl", "ltr", "ttb", "btt" };
static const int spg_dir_ids[4] = { S_DIR_RTL, S_DIR_LTR, S_DIR_TTB, S_DIR_BTT };
int          classic_spg_dir_count(void)     { return 4; }
const char * classic_spg_dir_code(int i)     { return i >= 0 && i < 4 ? spg_dir_codes[i] : "rtl"; }
int          classic_spg_dir_name_id(int i)  { return i >= 0 && i < 4 ? spg_dir_ids[i] : S_DIR_RTL; }

static void spectrogram_create(void)
{
    spg_root = lv_obj_create(root);
    lv_obj_remove_style_all(spg_root);
    lv_obj_set_size(spg_root, W, H);
    lv_obj_remove_flag(spg_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spg_root, LV_OBJ_FLAG_HIDDEN);
    int margin = ui_screen.kind == UI_SCREEN_TV ? H * 8 / 100 : H * 5 / 100;
    spg_x = W * 3 / 100; spg_w = W - 2 * spg_x; spg_y = margin; spg_h = H - 2 * margin;
    spg_step = px(6) > 2 ? px(6) : 2;
    spg_buf = calloc((size_t)spg_w * (size_t)spg_h, 4);
    image_dsc(&spg_dsc, spg_buf, spg_w, spg_h);
    spg_img = lv_image_create(spg_root);
    lv_image_set_src(spg_img, &spg_dsc);
    lv_obj_set_pos(spg_img, spg_x, spg_y);
}

static void spg_clear(void);

static void spectrogram_colours(void)
{
    uint32_t fallback[3] = { cur.ground, cur.bar, cur.cap };
    const uint32_t * g = cur.spg ? cur.spg : fallback; int n = cur.spg ? cur.nspg : 3;
    for(int i = 0; i < 256; i++) spg_lut[i] = 0xFF000000u | grad_at(g, n, i / 255.0f);
    spg_clear();
}

static void spectrogram_frame(void)
{
    if(!spg_buf) return;
    float col[VIZ_SPG_ROWS];
    if(!viz_spectrogram(col, VIZ_SPG_ROWS)) memset(col, 0, sizeof(col));
    uint32_t * p = (uint32_t *)spg_buf;
    size_t stride = (size_t)spg_w;
    if(spg_dir == SPG_RTL || spg_dir == SPG_LTR) {
        /* scroll sideways by a step, paint the new column at the entering edge */
        for(int y = 0; y < spg_h; y++) {
            uint32_t * row = p + (size_t)y * stride;
            int ri = (spg_h - 1 - y) * VIZ_SPG_ROWS / spg_h;                 /* the top row is the highest pitch */
            float v = col[ri]; int idx = (int)(v * 255); idx = idx < 0 ? 0 : idx > 255 ? 255 : idx;
            uint32_t c = spg_lut[idx];
            if(spg_dir == SPG_RTL) { memmove(row, row + spg_step, (size_t)(spg_w - spg_step) * 4); for(int x = spg_w - spg_step; x < spg_w; x++) row[x] = c; }
            else                   { memmove(row + spg_step, row, (size_t)(spg_w - spg_step) * 4); for(int x = 0; x < spg_step; x++) row[x] = c; }
        }
    }
    else {
        /* scroll up or down by a step of rows, paint the new rows at the entering edge; pitch runs left (low) to right (high) */
        size_t move = (size_t)(spg_h - spg_step) * stride * 4;
        int y0;
        if(spg_dir == SPG_TTB) { memmove(p + (size_t)spg_step * stride, p, move); y0 = 0; }
        else                   { memmove(p, p + (size_t)spg_step * stride, move); y0 = spg_h - spg_step; }
        uint32_t * first = p + (size_t)y0 * stride;
        for(int x = 0; x < spg_w; x++) {
            int ri = x * VIZ_SPG_ROWS / spg_w;
            float v = col[ri]; int idx = (int)(v * 255); idx = idx < 0 ? 0 : idx > 255 ? 255 : idx;
            first[x] = spg_lut[idx];
        }
        for(int y = 1; y < spg_step; y++) memcpy(first + (size_t)y * stride, first, stride * 4);
    }
    lv_obj_invalidate(spg_img);
}

static void spg_clear(void)
{
    if(spg_buf) { uint32_t * p = (uint32_t *)spg_buf; for(size_t i = 0; i < (size_t)spg_w * spg_h; i++) p[i] = spg_lut[0]; }
    lv_obj_invalidate(spg_img);
}

void classic_set_spg_dir(const char * code)
{
    spg_dir_t d = SPG_RTL;
    for(int i = 0; i < 4; i++) if(code && strcmp(code, spg_dir_codes[i]) == 0) d = (spg_dir_t)i;
    if(d == spg_dir) return;
    spg_dir = d;
    spg_clear();                                                          /* a fresh picture in the new direction */
    fprintf(stderr, "simpleton-ui: classic: spectrogram flows %s\n", spg_dir_codes[d]);
}

/* ------------------------------------------------------------ all of them */

static void apply_colours(void)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(cur.ground), 0);
    if(cur.scanlines && kind == CLASSIC_SPECTRUM) { make_ground(); lv_obj_set_style_bg_image_src(root, NULL, 0); lv_obj_set_style_bg_image_src(root, &ground_dsc, 0); }
    else lv_obj_set_style_bg_image_src(root, NULL, 0);
    switch(kind) {
        case CLASSIC_SPECTRUM:    spectrum_colours(); break;
        case CLASSIC_METERS:      meter_colours(&mt[0]); meter_colours(&mt[1]); break;
        case CLASSIC_SCOPE:       scope_colours(); break;
        case CLASSIC_SPECTROGRAM: spectrogram_colours(); break;
        default: break;
    }
}

static void frame(lv_timer_t * t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    switch(kind) {
        case CLASSIC_SPECTRUM:    spectrum_frame(now); break;
        case CLASSIC_METERS:      meters_frame(); break;
        case CLASSIC_SCOPE:       scope_frame(); break;
        case CLASSIC_SPECTROGRAM: spectrogram_frame(); break;
        default: return;
    }
    if(cur.dealt && strcmp(cur.code, "record") == 0 && now - last_track_ms > 1500) { last_track_ms = now; record_colours(false); }
}

static lv_obj_t * kind_root(classic_kind_t k)
{
    switch(k) { case CLASSIC_SPECTRUM: return sp_root; case CLASSIC_METERS: return mt_root; case CLASSIC_SCOPE: return sc_root; case CLASSIC_SPECTROGRAM: return spg_root; default: return NULL; }
}

static const char * kind_code(classic_kind_t k)
{
    switch(k) { case CLASSIC_SPECTRUM: return "spectrum"; case CLASSIC_METERS: return "meters"; case CLASSIC_SCOPE: return "scope"; case CLASSIC_SPECTROGRAM: return "spectrogram"; default: return "none"; }
}

void classic_create(lv_obj_t * parent)
{
    W = ui_screen.w; H = ui_screen.h;
    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, W, H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(root, true, 0);
    lv_obj_set_style_bg_image_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    spectrum_create();
    meters_create();
    scope_create();
    spectrogram_create();
}

bool classic_start(classic_kind_t k, const char * look)
{
    if(!root || k == CLASSIC_NONE) return false;
    if(kind != CLASSIC_NONE) classic_stop();
    if(!viz_start(VIZ_FEED)) { fprintf(stderr, "simpleton-ui: classic: no feed\n"); return false; }
    kind = k;
    set_look(look);
    apply_colours();
    last_track_ms = lv_tick_get();
    if(k == CLASSIC_SPECTRUM) spectrum_reset();
    if(k == CLASSIC_METERS) { mt[0].db = mt[1].db = -20; }
    if(k == CLASSIC_SCOPE) sc_gain = 1;
    if(k == CLASSIC_SPECTROGRAM) { char d[16]; config_read("viz-spectrogram-dir", d, sizeof(d), "rtl"); spg_dir = SPG_RTL; classic_set_spg_dir(d); }
    for(classic_kind_t o = CLASSIC_SPECTRUM; o <= CLASSIC_SPECTROGRAM; o++) { lv_obj_t * r = kind_root(o); if(r) { if(o == k) lv_obj_remove_flag(r, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN); } }
    lv_obj_remove_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root);
    if(!timer) timer = lv_timer_create(frame, FRAME_MS, NULL);
    lv_timer_set_period(timer, k == CLASSIC_SCOPE ? SCOPE_MS : k == CLASSIC_SPECTROGRAM ? SPG_MS : FRAME_MS);
    lv_timer_resume(timer);
    fprintf(stderr, "simpleton-ui: classic: %s, look %s (ground %06X, bars %06X, caps %06X)\n", kind_code(k), cur.code, cur.ground, cur.bar, cur.cap);
    return true;
}

void classic_set_look(const char * look)
{
    if(kind == CLASSIC_NONE) return;
    set_look(look);
    apply_colours();
    fprintf(stderr, "simpleton-ui: classic: look %s\n", cur.code);
}

void classic_stop(void)
{
    if(kind == CLASSIC_NONE) return;
    kind = CLASSIC_NONE;
    if(timer) lv_timer_pause(timer);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    viz_stop();
}

bool classic_active(void) { return kind != CLASSIC_NONE; }

classic_kind_t classic_kind_from_code(const char * code)
{
    if(!code) return CLASSIC_NONE;
    if(strcmp(code, "spectrum") == 0) return CLASSIC_SPECTRUM;
    if(strcmp(code, "meters") == 0) return CLASSIC_METERS;
    if(strcmp(code, "scope") == 0) return CLASSIC_SCOPE;
    if(strcmp(code, "spectrogram") == 0) return CLASSIC_SPECTROGRAM;
    return CLASSIC_NONE;
}
