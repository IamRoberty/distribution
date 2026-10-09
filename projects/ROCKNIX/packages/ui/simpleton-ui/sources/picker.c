/*
 * SimpletonOS UI - the browse picker and the jump pill (implementation).
 * See picker.h.
 *
 * Both faces are plain objects on the view's stage, hidden until opened:
 *
 *   pill    a small rounded chip, top centre of the list area, "< A >":
 *           the current jump letter; Left/Right steps it and the list
 *           behind follows at once
 *   panel   a compact card centred on the stage with the four value rows
 *           (view style, size, sort, alphabet picker). The focused row is
 *           full size, the others are reduced and dim, so it reads as an
 *           overlay rather than a page.
 *
 * Nothing here knows what a view style or a sort means: the caller hands
 * in names and gets indexes back, and is told of every change as it is
 * made (changes apply live and are the caller's to remember).
 */
#include "picker.h"
#include "strings.h"
#include "theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PICKER_MINIMIZE
#define PICKER_MINIMIZE 1              /* 1: the rows not in focus are drawn smaller */
#endif

typedef enum { ROW_STYLE, ROW_SIZE, ROW_SORT, ROW_STRIP, ROW_COUNT } row_t;

static ui_list_t lm;        /* the view's list area: where the overlay sits */
static ui_list_t mm;        /* the menu's measurements: how big it is drawn (the
                             * Settings pages' size, never the view's - Ian, 7 Oct) */
static lv_obj_t * stage;
static picker_mode_t mode = PICKER_NONE;
static picker_opts_t opts;
static bool          from_strip;      /* the panel was opened from the pill: Back returns there */
static void (*jump_cb)(int, void *);
static void (*change_cb)(const picker_opts_t *, void *);
static void (*close_cb)(bool, void *);
static void * cb_ctx;

/* pill */
static lv_obj_t * strip;              /* the pill object */
static lv_obj_t * pill_label;

/* panel */
static lv_obj_t * panel;
static lv_obj_t * rows[ROW_COUNT];
static lv_obj_t * row_names[ROW_COUNT];
static lv_obj_t * row_values[ROW_COUNT];      /* the "< value >" label */
static int focus_row;

/* ------------------------------------------------------------ labels */

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

/* Upper-case a letter where case exists (Latin, Greek, Cyrillic). */
static uint32_t upper(uint32_t c)
{
    if(c < 128) return (uint32_t)toupper((int)c);
    if(c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if(c == 0xFF) return 0x178;
    if(c >= 0x100 && c <= 0x17F) return (c & 1) && c != 0x131 ? c - 1 : c;
    if(c >= 0x1EA0 && c <= 0x1EF9) return c & ~1u;                /* Vietnamese pairs */
    if(c == 0x1A1) return 0x1A0;                                  /* ơ */
    if(c == 0x1B0) return 0x1AF;                                  /* ư */
    if(c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
    if(c >= 0x430 && c <= 0x44F) return c - 0x20;
    if(c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

/* Strip the accent off an upper-case Latin letter: the base letter, or c. */
static uint32_t base_latin(uint32_t c)
{
    static const struct { uint32_t from, to; char base; } ranges[] = {
        { 0xC0, 0xC5, 'A' }, { 0xC7, 0xC7, 'C' }, { 0xC8, 0xCB, 'E' }, { 0xCC, 0xCF, 'I' },
        { 0xD1, 0xD1, 'N' }, { 0xD2, 0xD6, 'O' }, { 0xD8, 0xD8, 'O' }, { 0xD9, 0xDC, 'U' }, { 0xDD, 0xDD, 'Y' },
        { 0x100, 0x104, 'A' }, { 0x106, 0x10C, 'C' }, { 0x10E, 0x110, 'D' }, { 0x112, 0x11A, 'E' },
        { 0x11C, 0x122, 'G' }, { 0x124, 0x126, 'H' }, { 0x128, 0x130, 'I' }, { 0x134, 0x134, 'J' },
        { 0x136, 0x136, 'K' }, { 0x139, 0x141, 'L' }, { 0x143, 0x147, 'N' }, { 0x14C, 0x150, 'O' },
        { 0x152, 0x152, 'O' }, { 0x154, 0x158, 'R' }, { 0x15A, 0x160, 'S' }, { 0x162, 0x166, 'T' },
        { 0x168, 0x172, 'U' }, { 0x174, 0x174, 'W' }, { 0x176, 0x178, 'Y' }, { 0x179, 0x17D, 'Z' },
        { 0x1A0, 0x1A0, 'O' }, { 0x1AF, 0x1AF, 'U' },
        { 0x1EA0, 0x1EB6, 'A' }, { 0x1EB8, 0x1EC6, 'E' }, { 0x1EC8, 0x1ECA, 'I' },
        { 0x1ECC, 0x1EE2, 'O' }, { 0x1EE4, 0x1EF0, 'U' }, { 0x1EF2, 0x1EF8, 'Y' },
    };
    for(size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
        if(c >= ranges[i].from && c <= ranges[i].to) return (uint32_t)ranges[i].base;
    return c;
}

/* 0 Latin, 1 Cyrillic, 2 "#", 3 other, 4 "?" */
static int label_group(uint32_t c)
{
    if(c == '#') return 2;
    if(c == '?') return 4;
    if(c >= 'A' && c <= 'Z') return 0;
    if((c >= 0x400 && c <= 0x4FF)) return 1;
    return 3;
}

static uint32_t fold_cp(const char * name)
{
    const char * s = name;
    uint32_t c = 0;
    /* skip blanks and quotes before the first real character */
    while(*s) {
        const char * before = s;
        c = utf8_next(&s);
        if(c == ' ' || c == '\t' || c == '"' || c == '\'' || c == 0x2018 || c == 0x2019 || c == 0x201C || c == 0x201D) continue;
        if(c == 0) { s = before; break; }
        break;
    }
    if(!c) return '#';
    c = upper(c);
    c = base_latin(c);
    if(c < 128) return (c >= 'A' && c <= 'Z') ? c : '#';
    return c;
}

void picker_fold(const char * name, char out[PICKER_LABEL_LEN])
{
    uint32_t c = fold_cp(name ? name : "");
    int n = utf8_put(out, c);
    out[n] = 0;
}

static int label_cmp(const void * a, const void * b)
{
    const char * x = a, * y = b;
    const char * px = x, * py = y;
    uint32_t cx = utf8_next(&px), cy = utf8_next(&py);
    int gx = label_group(cx), gy = label_group(cy);
    if(gx != gy) return gx - gy;
    return cx < cy ? -1 : cx > cy ? 1 : 0;
}

void picker_labels_from_names(picker_opts_t * o, const char * const * names, int count, bool end_section)
{
    o->label_count = 0;
    for(int i = 0; i < count && o->label_count < PICKER_MAX_LABELS - 1; i++) {
        char l[PICKER_LABEL_LEN];
        picker_fold(names[i], l);
        bool seen = false;
        for(int k = 0; k < o->label_count && !seen; k++) seen = strcmp(o->labels[k], l) == 0;
        if(!seen) snprintf(o->labels[o->label_count++], PICKER_LABEL_LEN, "%s", l);
    }
    qsort(o->labels, (size_t)o->label_count, PICKER_LABEL_LEN, label_cmp);
    if(end_section && o->label_count < PICKER_MAX_LABELS) snprintf(o->labels[o->label_count++], PICKER_LABEL_LEN, "?");
    if(o->jump >= o->label_count) o->jump = o->label_count ? 0 : -1;
}

int picker_label_of(const picker_opts_t * o, const char * name)
{
    char l[PICKER_LABEL_LEN];
    picker_fold(name, l);
    for(int k = 0; k < o->label_count; k++) if(strcmp(o->labels[k], l) == 0) return k;
    return -1;
}

/* -------------------------------------------------------------- pill */

static int pill_h(void) { int h = mm.row_h * 3 / 4; return h < lm.row_h ? h : lm.row_h; }

static void strip_render(void)
{
    const char * letter = (opts.label_count && opts.jump >= 0) ? opts.labels[opts.jump] : "-";
    char txt[48];
    snprintf(txt, sizeof(txt), LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, letter);
    lv_label_set_text(pill_label, txt);
    lv_obj_update_layout(pill_label);
    int h = pill_h();
    int w = lv_obj_get_width(pill_label) + h;          /* half a height of air each side */
    lv_obj_set_size(strip, w, h);
    lv_obj_set_pos(strip, lm.x + (lm.w - w) / 2, lm.y + (lm.row_h - h) / 2);     /* in the view's top row band */
    lv_obj_center(pill_label);
}

/* ------------------------------------------------------------- panel */

static bool row_steppable(row_t r)
{
    switch(r) {
        case ROW_STYLE: return opts.style_count > 1;
        case ROW_SIZE:  return true;
        case ROW_SORT:  return opts.sort_count > 1;
        case ROW_STRIP: return true;
        default: return false;
    }
}

static void panel_value_text(row_t r, char * out, size_t len)
{
    const char * v = "";
    switch(r) {
        case ROW_STYLE: v = opts.style_count ? opts.styles[opts.style] : ""; break;
        case ROW_SIZE:  v = ui_size_name(opts.size); break;
        case ROW_SORT:  v = opts.sort_count ? opts.sorts[opts.sort] : ""; break;
        case ROW_STRIP: v = opts.strip ? T(S_SET_ON) : T(S_SET_OFF); break;
        default: break;
    }
    if(row_steppable(r)) snprintf(out, len, LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, v);
    else snprintf(out, len, "%s", v);
}

/* Row geometry: the focused row is a full list row; with PICKER_MINIMIZE the
 * others are three-quarter height in the chrome font. */
static int row_height(bool on) { return (on || !PICKER_MINIMIZE) ? mm.row_h : mm.row_h * 3 / 4; }
static const lv_font_t * row_font(bool on) { return (on || !PICKER_MINIMIZE) ? ui_font_px(mm.font_px) : ui_font_px(mm.chrome_font_px); }

static int panel_w(void) { return mm.w * 3 / 4; }
static int panel_pad(void) { return mm.margin; }

static void panel_refresh(void)
{
    int pad = panel_pad();
    int y = pad;
    int inner_w = panel_w() - 2 * pad;
    for(int r = 0; r < ROW_COUNT; r++) {
        bool on = (r == focus_row);
        int h = row_height(on);
        lv_obj_set_size(rows[r], inner_w, h);
        lv_obj_set_pos(rows[r], pad, y);
        lv_obj_set_style_text_font(rows[r], row_font(on), 0);
        lv_obj_set_style_bg_opa(rows[r], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row_names[r], lv_color_hex(on ? UI_COLOR_FOCUS_FG : UI_COLOR_DIM), 0);
        lv_obj_set_style_text_color(row_values[r], lv_color_hex(on ? UI_COLOR_FOCUS_FG : UI_COLOR_DIM), 0);
        char v[64];
        panel_value_text((row_t)r, v, sizeof(v));
        lv_label_set_text(row_values[r], v);
        lv_obj_align(row_names[r], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_align(row_values[r], LV_ALIGN_RIGHT_MID, 0, 0);
        y += h + mm.row_gap;
    }
    int panel_h = y - mm.row_gap + pad;
    lv_obj_set_size(panel, panel_w(), panel_h);
    /* centred on the stage's list area */
    lv_obj_set_pos(panel, lm.x + (lm.w - panel_w()) / 2, lm.y + (lm.h - panel_h) / 2);
}

static void panel_build(void)
{
    lv_obj_clean(panel);
    const char * names[ROW_COUNT] = { T(S_PICKER_STYLE), T(S_PICKER_SIZE), T(S_PICKER_SORT), T(S_PICKER_STRIP) };
    for(int r = 0; r < ROW_COUNT; r++) {
        lv_obj_t * row = lv_obj_create(panel);
        lv_obj_remove_style_all(row);
        lv_obj_set_style_radius(row, mm.row_radius, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
        lv_obj_set_style_pad_hor(row, mm.margin, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        rows[r] = row;
        row_names[r] = lv_label_create(row);
        lv_label_set_text(row_names[r], names[r]);
        row_values[r] = lv_label_create(row);
    }
    panel_refresh();
}

/* ------------------------------------------------------------ public */

static void show(picker_mode_t m);

void picker_attach(lv_obj_t * st, const ui_list_t * m)
{
    if(strip) {
        /* built already: move both faces to this view, closed */
        show(PICKER_NONE);
        lv_obj_set_parent(strip, st);
        lv_obj_set_parent(panel, st);
        stage = st;
        picker_relayout(m);
        return;
    }
    stage = st;
    lm = *m;
    ui_list_metrics(UI_SIZE_DEFAULT, &mm);

    strip = lv_obj_create(stage);
    lv_obj_remove_style_all(strip);
    lv_obj_set_style_bg_color(strip, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(strip, mm.margin, 0);
    lv_obj_set_style_shadow_opa(strip, LV_OPA_50, 0);
    lv_obj_set_style_text_color(strip, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
    pill_label = lv_label_create(strip);

    panel = lv_obj_create(stage);
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, mm.row_radius * 2, 0);
    lv_obj_set_style_shadow_width(panel, mm.margin, 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_60, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    picker_relayout(m);
}

void picker_relayout(const ui_list_t * m)
{
    lm = *m;
    lv_obj_set_style_text_font(strip, ui_font_px(mm.font_px), 0);
    if(mode == PICKER_STRIP) strip_render();
    if(mode == PICKER_FULL) panel_build();
}

picker_mode_t picker_mode(void) { return mode; }

static void show(picker_mode_t m)
{
    mode = m;
    if(m == PICKER_STRIP) {
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
        strip_render();
        lv_obj_remove_flag(strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(strip);
    }
    else if(m == PICKER_FULL) {
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
        panel_build();
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
    }
    else {
        lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void picker_open(picker_mode_t m, const picker_opts_t * o,
                 void (*on_jump)(int, void *),
                 void (*on_change)(const picker_opts_t *, void *),
                 void (*on_close)(bool, void *), void * ctx)
{
    opts = *o;
    if(opts.jump < 0 && opts.label_count) opts.jump = 0;
    jump_cb = on_jump;
    change_cb = on_change;
    close_cb = on_close;
    cb_ctx = ctx;
    focus_row = 0;
    from_strip = false;
    show(m);
}

void picker_close(void)
{
    show(PICKER_NONE);
}

const picker_opts_t * picker_opts(void) { return &opts; }

static void finish(bool back)
{
    show(PICKER_NONE);
    if(close_cb) close_cb(back, cb_ctx);
}

/* The panel closes: back to the pill it came from (if the alphabet picker
 * is still on), else away. */
static void panel_done(void)
{
    if(from_strip && opts.strip) { from_strip = false; show(PICKER_STRIP); }
    else finish(false);
}

static void step_jump(int dir)
{
    if(opts.label_count < 1) return;
    opts.jump = ((opts.jump + dir) % opts.label_count + opts.label_count) % opts.label_count;
    if(jump_cb) jump_cb(opts.jump, cb_ctx);
}

static void step_row(int dir)
{
    switch((row_t)focus_row) {
        case ROW_STYLE: if(opts.style_count > 1) opts.style = (opts.style + dir + opts.style_count) % opts.style_count; break;
        case ROW_SIZE: {
            /* only the sizes this screen offers */
            int s = (int)opts.size;
            for(int tries = 0; tries < UI_SIZE_COUNT; tries++) {
                s = (s + dir + UI_SIZE_COUNT) % UI_SIZE_COUNT;
                if(ui_size_offered((ui_size_t)s)) break;
            }
            opts.size = (ui_size_t)s;
            break;
        }
        case ROW_SORT:  if(opts.sort_count > 1) opts.sort = (opts.sort + dir + opts.sort_count) % opts.sort_count; break;
        case ROW_STRIP: opts.strip = !opts.strip; break;
        default: break;
    }
    if(change_cb) change_cb(&opts, cb_ctx);      /* applies live; may relayout the panel */
    if(mode == PICKER_FULL) panel_refresh();
}

void picker_handle_action(ui_action_t a)
{
    if(mode == PICKER_STRIP) {
        switch(a) {
            case ACT_LEFT:  step_jump(-1); strip_render(); break;
            case ACT_RIGHT: step_jump(+1); strip_render(); break;
            /* the pill sits over the top of the list: Down goes into the
             * list (keeps the place, like Select), Up opens the panel */
            case ACT_SELECT:
            case ACT_DOWN:  finish(false); break;
            case ACT_UP:    from_strip = true; focus_row = 0; show(PICKER_FULL); break;
            case ACT_BACK:  finish(true); break;
            default: break;
        }
        return;
    }
    if(mode == PICKER_FULL) {
        switch(a) {
            case ACT_UP:    if(focus_row > 0) focus_row--; panel_refresh(); break;
            case ACT_DOWN:  if(focus_row < ROW_COUNT - 1) focus_row++; panel_refresh(); break;
            case ACT_LEFT:  step_row(-1); break;
            case ACT_RIGHT: step_row(+1); break;
            case ACT_SELECT:
                /* "that one": on to the next row; from the last row, done */
                if(focus_row < ROW_COUNT - 1) { focus_row++; panel_refresh(); }
                else panel_done();
                break;
            case ACT_BACK:
            case ACT_SETTINGS: panel_done(); break;
            default: break;
        }
    }
}

void picker_toggle_panel(void)
{
    if(mode == PICKER_FULL) panel_done();
    else if(mode == PICKER_STRIP) { from_strip = true; focus_row = 0; show(PICKER_FULL); }
}

bool picker_from_strip(void) { return from_strip; }
