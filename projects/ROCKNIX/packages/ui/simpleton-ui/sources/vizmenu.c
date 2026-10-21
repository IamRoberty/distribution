/*
 * SimpletonOS UI - a visualizer's menu over it (implementation). See
 * vizmenu.h. The box is the object menu's sign (0.25): square, the keyline
 * inside, the page's colours turned round, a caps header and a rule; one
 * row per setting (0.33a: the Spectrogram's direction made it two), Up and
 * Down between them, the focused row's value in the selected colour.
 */
#include "vizmenu.h"
#include "classic.h"
#include "config.h"
#include "layout.h"
#include "strings.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OPTS 2

typedef struct { char key[48]; int label_id; int n; char codes[12][16]; char names[12][48]; int index; } option_t;

static lv_obj_t * sheet, * box, * keyline, * header, * rule, * name_l[MAX_OPTS], * value_l[MAX_OPTS];
static saver_kind_t kind;
static option_t opts[MAX_OPTS];
static int nopts, focus;
static bool active;

static void add_choice_opt(const char * key, int label_id, const char * def, int n, const char * const * codes, const int * name_ids, const char * const * names)
{
    if(nopts >= MAX_OPTS) return;
    option_t * o = &opts[nopts++];
    memset(o, 0, sizeof(*o));
    snprintf(o->key, sizeof(o->key), "%s", key);
    o->label_id = label_id;
    char cur[32]; config_read(key, cur, sizeof(cur), def);
    for(int i = 0; i < n && i < 12; i++) {
        snprintf(o->codes[i], sizeof(o->codes[0]), "%s", codes[i]);
        snprintf(o->names[i], sizeof(o->names[0]), "%s", name_ids ? T((str_id_t)name_ids[i]) : names[i]);
        if(strcmp(cur, codes[i]) == 0) o->index = i;
    }
    o->n = n < 12 ? n : 12;
}

static void fill_options(saver_kind_t k)
{
    nopts = 0; focus = 0;
    const char * code = saver_kind_code(k);
    if(classic_kind_from_code(code) != CLASSIC_NONE) {
        char key[48]; snprintf(key, sizeof(key), "viz-%s-look", code);
        const char * codes[12]; int ids[12]; int n = classic_look_count() < 12 ? classic_look_count() : 12;
        for(int i = 0; i < n; i++) { codes[i] = classic_look_code(i); ids[i] = classic_look_name_id(i); }
        add_choice_opt(key, S_SET_LOOK, "archer", n, codes, ids, NULL);
        if(k == SAVER_SPECTROGRAM) {
            int dn = classic_spg_dir_count(); const char * dc[4]; int di[4];
            for(int i = 0; i < dn; i++) { dc[i] = classic_spg_dir_code(i); di[i] = classic_spg_dir_name_id(i); }
            add_choice_opt("viz-spectrogram-dir", S_SET_DIRECTION, "rtl", dn, dc, di, NULL);
        }
    }
    else if(k == SAVER_BOUNCE && config_read_bool("developer", false)) {
        static const char * const v[] = { "10", "50", "100", "250", "500", "1000", "2000", "5000" };
        add_choice_opt("viz-bounce-n", 0, "1000", 8, v, NULL, v);
    }
}

bool vizmenu_has_options(saver_kind_t k)
{
    fill_options(k);
    return nopts > 0;
}

static void render(void)
{
    ui_colours_t c = ui_colours_inverse();
    int w = PX(440);
    if(w > ui_screen.w - PX(16)) w = ui_screen.w - PX(16);
    int in = ui_sign_inset(w) + PX(14);
    int rowh = PX(52);
    int h = 2 * in + PX(40) + rowh * nopts;                             /* the header, the rule, the rows */
    lv_obj_set_size(box, w, h);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -PX(40));                 /* low, so the picture shows */
    char caps[120];
    int tid = kind == SAVER_SPECTRUM ? S_SET_SAVER_SPECTRUM : kind == SAVER_METERS ? S_SET_SAVER_METERS : kind == SAVER_SCOPE ? S_SET_SAVER_SCOPE :
              kind == SAVER_SPECTROGRAM ? S_SET_SAVER_SPECTROGRAM : kind == SAVER_BOUNCE ? S_SET_SAVER_BOUNCE : S_HOME_VISUALIZATION;
    ui_caps(T((str_id_t)tid), caps, sizeof(caps));
    lv_label_set_text(header, caps);
    lv_obj_set_size(header, w - 2 * in, lv_font_get_line_height(ui_font_caps(20)) + 2);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, in - PX(8), in - PX(4));
    lv_obj_set_pos(rule, in - PX(8), in + PX(26));
    lv_obj_set_width(rule, w - 2 * in);
    lv_obj_set_style_text_color(header, lv_color_hex(c.ink), 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(c.ink), 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(c.ground), 0);
    ui_sign_fit(keyline);
    ui_sign_colour(keyline, c.ink);
    for(int i = 0; i < MAX_OPTS; i++) {
        if(i >= nopts) { lv_obj_add_flag(name_l[i], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(value_l[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(name_l[i], LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(value_l[i], LV_OBJ_FLAG_HIDDEN);
        const option_t * o = &opts[i];
        bool f = i == focus;
        if(o->label_id) { ui_caps(T((str_id_t)o->label_id), caps, sizeof(caps)); lv_label_set_text(name_l[i], caps); }
        else lv_label_set_text(name_l[i], "");
        char val[80];
        if(f && nopts > 0) snprintf(val, sizeof(val), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, o->names[o->index]);
        else snprintf(val, sizeof(val), "%s", o->names[o->index]);
        lv_label_set_text(value_l[i], val);
        lv_obj_set_style_text_font(value_l[i], f ? ui_font(28 * UI_GROW_PCT / 100) : ui_font(28), 0);
        lv_obj_set_style_text_color(name_l[i], lv_color_hex(c.ink), 0);
        lv_obj_set_style_text_color(value_l[i], lv_color_hex(f ? c.sel : c.ink), 0);
        int y = in + PX(44) + rowh * i;
        lv_obj_align(name_l[i], LV_ALIGN_TOP_LEFT, in - PX(8), y + PX(6));
        lv_obj_align(value_l[i], LV_ALIGN_TOP_RIGHT, -(in - PX(8)), y);
    }
}

static void apply(const option_t * o)
{
    config_write(o->key, o->codes[o->index]);
    fprintf(stderr, "simpleton-ui: vizmenu: %s = %s\n", o->key, o->codes[o->index]);
    /* the picture behind follows at once */
    if(strcmp(o->key, "viz-spectrogram-dir") == 0) classic_set_spg_dir(o->codes[o->index]);
    else if(strstr(o->key, "-look")) classic_set_look(o->codes[o->index]);
    else if(kind == SAVER_BOUNCE) saver_set_bounce_every(atoi(o->codes[o->index]));
}

void vizmenu_create(void)
{
    sheet = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(sheet, 0, 0);
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    box = lv_obj_create(sheet);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, PX(8), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    keyline = ui_sign(box);
    header = lv_label_create(box);
    lv_obj_set_style_text_font(header, ui_font_caps(20), 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_DOT);
    rule = ui_rule(box, 0, 0, PX(100));
    for(int i = 0; i < MAX_OPTS; i++) {
        name_l[i] = lv_label_create(box);
        lv_obj_set_style_text_font(name_l[i], ui_font_caps(22), 0);
        value_l[i] = lv_label_create(box);
        lv_obj_set_style_text_font(value_l[i], ui_font(28), 0);
    }
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

void vizmenu_open(saver_kind_t k)
{
    if(!sheet) return;
    kind = k;
    fill_options(k);
    if(!nopts) return;
    active = true;
    render();
    lv_obj_remove_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(sheet);
}

void vizmenu_close(void)
{
    if(!active) return;
    active = false;
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
}

bool vizmenu_active(void) { return active; }

void vizmenu_handle_action(ui_action_t a)
{
    switch(a) {
        case ACT_UP:
        case ACT_DOWN:
            if(nopts < 2) return;
            focus = (focus + (a == ACT_DOWN ? 1 : -1) + nopts) % nopts;
            render();
            return;
        case ACT_LEFT:
        case ACT_RIGHT: {
            option_t * o = &opts[focus];
            if(o->n < 2) return;
            o->index = (o->index + (a == ACT_RIGHT ? 1 : -1) + o->n) % o->n;
            apply(o);
            render();
            return;
        }
        case ACT_SELECT:
        case ACT_BACK:
        case ACT_MENU:
        case ACT_SETTINGS:                                    /* the button that opened it closes it */
            vizmenu_close();                                  /* the visualizer stays (Ian: select closes the box) */
            return;
        default:
            return;                                           /* the rest is swallowed while the box is up */
    }
}
