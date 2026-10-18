/*
 * SimpletonOS UI - stage helper. See theme.h.
 *
 * ui_font() lives in fonts.c (0.9); the screen and stage sizes live in
 * layout.c (0.11).
 */
#include "theme.h"
lv_obj_t * ui_stage_create(lv_obj_t * screen)
{
    lv_obj_t * s = lv_obj_create(screen);
    lv_obj_remove_style_all(s);                 /* no bg, border, padding */
    lv_obj_set_size(s, ui_screen.stage, ui_screen.stage);
    lv_obj_center(s);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
    return s;
}

#include "tilestyle.h"
#include "palette.h"
#include "placeholder.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------- the three colours */

static ui_colours_t cur = { UI_COLOR_BG, UI_COLOR_FG, UI_COLOR_FOCUS_BG };
static char   cur_scope[32];
static int    last_set = -1;
static unsigned deals;

uint32_t ui_mix(uint32_t a, uint32_t b, int pct_b)
{
    int pa = 100 - pct_b;
    uint32_t r = (((a >> 16) & 255) * pa + ((b >> 16) & 255) * pct_b) / 100;
    uint32_t g = (((a >> 8) & 255) * pa + ((b >> 8) & 255) * pct_b) / 100;
    uint32_t bl = ((a & 255) * pa + (b & 255) * pct_b) / 100;
    return (r << 16) | (g << 8) | bl;
}

static bool light(uint32_t c) { return tilestyle_contrast(c, 0x000000) > tilestyle_contrast(c, 0xFFFFFF); }

/* The ink for a ground, as the list pages pick theirs (listpage.c 0.19):
 * the set's most colourful colour that clears the list floor, else the
 * cream or the dark ink, whichever reads. 0 = nothing reads. */
static uint32_t pick_ink(uint32_t bg, const uint32_t * set, int n)
{
    double floor = tilestyle_list_contrast();
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0, dark = tilestyle_dark_ink();
    uint32_t best = 0; double bests = -1;
    for(int k = 0; k < n; k++) {
        if(set[k] == bg || tilestyle_contrast(bg, set[k]) < floor) continue;
        double sat = tilestyle_saturation(set[k]);
        bool strong = sat >= 0.35 || (light(bg) != light(set[k]) && tilestyle_saturation(bg) >= 0.35);
        if(!strong) continue;
        if(sat > bests) { bests = sat; best = set[k]; }
    }
    if(best) return best;
    if(tilestyle_contrast(bg, cream) >= floor && tilestyle_contrast(bg, cream) >= tilestyle_contrast(bg, dark)) return cream;
    if(tilestyle_contrast(bg, dark) >= floor) return dark;
    if(tilestyle_contrast(bg, cream) >= floor) return cream;
    return 0;
}

/* The selected colour: a set colour apart from the ink and the ground that
 * reads on the ground (the theme's floor; the list floor in high contrast). */
static uint32_t pick_sel(uint32_t bg, uint32_t ink, const uint32_t * set, int n, unsigned seed)
{
    double floor = tilestyle_high_contrast() ? tilestyle_list_contrast() : tilestyle_min_contrast();
    uint32_t cand[16]; int nc = 0;
    for(int k = 0; k < n && nc < 16; k++) {
        uint32_t c = set[k];
        if(c == bg || c == ink || tilestyle_distance(c, bg) < 70 || tilestyle_distance(c, ink) < 70) continue;
        if(tilestyle_contrast(bg, c) < floor) continue;
        cand[nc++] = c;
    }
    if(nc) return cand[seed % (unsigned)nc];
    uint32_t a = tilestyle_accent();
    if(a && tilestyle_distance(a, ink) >= 70 && tilestyle_contrast(bg, a) >= floor) return a;
    return ink;
}

static void deal(void)
{
    int nsets = tilestyle_set_count();
    unsigned seed = (unsigned)lv_tick_get() * 2654435761u + ++deals * 40503u;
    if(nsets > 0) {
        for(int tries = 0; tries < nsets * 4; tries++) {
            int si = (int)((seed + (unsigned)tries * 7919u) % (unsigned)nsets);
            if(si == last_set && nsets > 1) continue;
            uint32_t set[16]; int n = tilestyle_set(si, set, 16);
            if(n < 2) continue;
            unsigned r = seed ^ ((unsigned)tries * 0x9E3779B9u);
            for(int t = 0; t < n; t++) {
                uint32_t bg = set[(r + (unsigned)t) % (unsigned)n];
                uint32_t ink = pick_ink(bg, set, n);
                if(!ink) continue;
                uint32_t sel = pick_sel(bg, ink, set, n, r >> 4);
                cur.ground = bg; cur.ink = ink; cur.sel = sel;
                last_set = si;
                fprintf(stderr, "simpleton-ui: colours: %s -> set %d: ground %06X ink %06X selected %06X\n", cur_scope, si, bg, ink, sel);
                return;
            }
        }
    }
    cur.ground = UI_COLOR_BG; cur.ink = UI_COLOR_FG;
    cur.sel = tilestyle_accent() ? tilestyle_accent() : UI_COLOR_FOCUS_BG;
}

void ui_colours_visit(const char * scope)
{
    if(!scope) scope = "";
    if(strcmp(scope, cur_scope) == 0) return;
    snprintf(cur_scope, sizeof(cur_scope), "%s", scope);
    deal();
}
void ui_colours_redeal(void) { cur_scope[0] = 0; }
void ui_colours_set(uint32_t ground, uint32_t ink, uint32_t sel)
{
    snprintf(cur_scope, sizeof(cur_scope), "page");
    cur.ground = ground; cur.ink = ink; cur.sel = sel;
}
const ui_colours_t * ui_colours(void) { return &cur; }
uint32_t ui_ground(void)     { return cur.ground; }
uint32_t ui_ink(void)        { return cur.ink; }
uint32_t ui_dim(void)        { return ui_mix(cur.ground, cur.ink, tilestyle_high_contrast() ? 85 : 62); }
uint32_t ui_faint(void)      { return ui_mix(cur.ground, cur.ink, 28); }
uint32_t ui_accent(void)     { return cur.sel; }
uint32_t ui_accent_ink(void)
{
    /* the ground, when it reads on the selected colour (a toast is a small
     * sign turned inside out); else the cream or the dark ink */
    if(tilestyle_contrast(cur.sel, cur.ground) >= 3.0) return cur.ground;
    uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0, dark = tilestyle_dark_ink();
    return tilestyle_contrast(cur.sel, cream) >= tilestyle_contrast(cur.sel, dark) ? cream : dark;
}

/* ------------------------------------------------------------ signs */

int ui_sign_inset(int side) { return side * 34 / 1000; }               /* 24 on the handheld stage, 12 on a pop-up */
int ui_sign_line(int side)  { (void)side; return PX(2) < 1 ? 1 : PX(2); }  /* thin (0.25a: the thick line was "too close to the text") */

void ui_sign_chrome(const ui_list_t * lm, ui_sign_chrome_t * o)
{
    /* air all round (0.25a, Ian): the keyline a little in from the edge,
     * the header, the rows and the hints on one left edge clear of the
     * battery corner, the same gap under the rows as above the header */
    int in = ui_sign_inset(UI_BASE), gap = PX(32);
    o->header_x = PX(72);
    o->header_y = in + gap;
    o->header_w = UI_BASE - 2 * o->header_x;
    o->header_h = lm->chrome_font_px + PX(10);
    o->rule_y   = o->header_y + o->header_h;
    o->list_y   = o->rule_y + PX(16);
    o->footer_y = UI_BASE - in - gap - lm->footer_h;
    o->list_h   = o->footer_y - PX(16) - o->list_y;
}

ui_colours_t ui_colours_inverse(void)
{
    ui_colours_t inv = { cur.ink, cur.ground, cur.sel };
    double floor = tilestyle_high_contrast() ? tilestyle_list_contrast() : tilestyle_min_contrast();
    if(tilestyle_contrast(inv.ground, cur.sel) < floor || tilestyle_distance(cur.sel, inv.ink) < 70) {
        uint32_t cream = tilestyle_cream() ? tilestyle_cream() : 0xFAEFD0, dark = tilestyle_dark_ink();
        uint32_t pick = tilestyle_contrast(inv.ground, cream) >= tilestyle_contrast(inv.ground, dark) ? cream : dark;
        inv.sel = tilestyle_distance(pick, inv.ink) >= 70 ? pick : inv.ink;
    }
    return inv;
}

lv_obj_t * ui_sign(lv_obj_t * box)
{
    lv_obj_t * k = lv_obj_create(box);
    lv_obj_remove_style_all(k);
    lv_obj_add_flag(k, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(k, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_opa(k, LV_OPA_COVER, 0);
    ui_sign_fit(k);
    lv_obj_move_background(k);
    return k;
}

void ui_sign_fit(lv_obj_t * k)
{
    if(!k) return;
    lv_obj_t * box = lv_obj_get_parent(k);
    lv_obj_update_layout(box);
    int w = lv_obj_get_width(box), h = lv_obj_get_height(box);
    int side = w < h ? w : h;
    int in = ui_sign_inset(side), bw = ui_sign_line(side);
    int pl = lv_obj_get_style_pad_left(box, 0), pt = lv_obj_get_style_pad_top(box, 0);
    lv_obj_set_pos(k, in - pl, in - pt);
    lv_obj_set_size(k, w - 2 * in, h - 2 * in);
    lv_obj_set_style_border_width(k, bw, 0);
    lv_obj_set_style_border_color(k, lv_color_hex(cur.ink), 0);
}

lv_obj_t * ui_rule(lv_obj_t * parent, int x, int y, int w)
{
    lv_obj_t * r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_add_flag(r, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, PX(2) < 1 ? 1 : PX(2));
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    ui_rule_colour(r);
    return r;
}
void ui_rule_colour(lv_obj_t * r) { if(r) lv_obj_set_style_bg_color(r, lv_color_hex(cur.ink), 0); }
void ui_sign_colour(lv_obj_t * k, uint32_t colour) { if(k) lv_obj_set_style_border_color(k, lv_color_hex(colour), 0); }

const lv_font_t * ui_font_caps(int px)
{
    bool caps = true;
    const char * f = placeholder_line_font(2, &caps);      /* the theme's bold line (font 2) */
    return ui_font_face_px(f && f[0] ? f : "Jost-Bold.ttf", px, true);
}

void ui_caps(const char * text, char * out, size_t len)
{
    size_t i = 0;
    for(const char * p = text ? text : ""; *p && i + 1 < len; p++)
        out[i++] = ((unsigned char)*p < 128) ? (char)toupper((unsigned char)*p) : *p;
    out[i] = 0;
}
