/*
 * SimpletonOS UI - the Settings hub (implementation). See settings.h.
 *
 * A page is a short list of rows, built fresh each time it is shown from
 * the settings files, so the screen never holds a stale value:
 *
 *   page    -> opens another page            (the hub's groups and views)
 *   toggle  -> On / Off                      (Select or Left/Right flips it)
 *   choice  -> one of a few values           (Left/Right steps, Select too)
 *   action  -> does something now            (Select)
 *   text    -> a dim line, not selectable    (dividers, "nothing here yet")
 *   hub     -> "All settings"                (back to the hub)
 *
 * Rows share the browser's list measurements (L on this screen), with the
 * value on the right in the same row. The focused row is the solid block,
 * as everywhere.
 */
#include "settings.h"
#include "config.h"
#include "home.h"
#include "mpdc.h"
#include "strings.h"
#include "theme.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIMPLETON_VERSION
#define SIMPLETON_VERSION "dev"
#endif

typedef enum { ROW_PAGE, ROW_TOGGLE, ROW_CHOICE, ROW_ACTION, ROW_TEXT, ROW_HUB } row_kind_t;
typedef enum { CH_NONE, CH_THEME, CH_LANGUAGE, CH_SIZE, CH_SORT, CH_STYLE } choice_kind_t;

#define MAX_ROWS     32
#define MAX_CHOICES  32

typedef struct {
    row_kind_t    kind;
    char          label[128];
    char          value[96];
    char          page[32];        /* ROW_PAGE: where it goes            */
    char          key[48];         /* ROW_TOGGLE / ROW_CHOICE: the file  */
    bool          def;             /* ROW_TOGGLE: default when no file   */
    choice_kind_t choice;
    int           index;           /* ROW_CHOICE: current choice          */
    int           action;          /* ROW_ACTION                          */
    lv_obj_t *    obj;
    lv_obj_t *    val_label;
} row_t;

enum { ACT_SCAN = 1 };

typedef struct { char code[32]; char name[96]; } choice_t;

static lv_obj_t * scr, * header, * list_box, * footer;
static ui_list_t lm;
static row_t rows[MAX_ROWS];
static int row_count, focus;
static int hub_focus;                /* the hub row a page was opened from: Back lands on it */
static char page[32] = "hub";
static bool from_view;
static bool active;
static void (*restart_cb)(const char * page);
static void (*view_changed_cb)(const char * view);
static choice_t choices[MAX_CHOICES];
static int choice_count;

/* ------------------------------------------------------- choice lists */

static int cmp_choice(const void * a, const void * b) { return strcmp(((const choice_t *)a)->code, ((const choice_t *)b)->code); }

/* The files <share>/<sub>/<code><ext> -> codes; `name` from a "name " line
 * (themes) or a "lang.name =" line (languages). */
static void scan_choices(const char * sub, const char * ext, bool theme_style)
{
    choice_count = 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", config_share_dir(), sub);
    DIR * d = opendir(dir);
    if(!d) return;
    struct dirent * e;
    size_t el = strlen(ext);
    while((e = readdir(d)) && choice_count < MAX_CHOICES) {
        size_t n = strlen(e->d_name);
        if(n <= el || strcmp(e->d_name + n - el, ext) != 0 || e->d_name[0] == '.') continue;
        choice_t * c = &choices[choice_count];
        char code[32];
        snprintf(code, sizeof(code), "%.*s", (int)(n - el), e->d_name);
        snprintf(c->code, sizeof(c->code), "%s", code);
        snprintf(c->name, sizeof(c->name), "%s", code);
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        FILE * f = fopen(path, "r");
        if(f) {
            char line[256];
            while(fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\r\n")] = 0;
                if(theme_style) { if(strncmp(line, "name ", 5) == 0) { snprintf(c->name, sizeof(c->name), "%.90s", line + 5); break; } }
                else {
                    char * eq = strchr(line, '=');
                    if(!eq || strncmp(line, "lang.name", 9) != 0) continue;
                    char * v = eq + 1;
                    while(*v == ' ' || *v == '\t') v++;
                    if(*v) snprintf(c->name, sizeof(c->name), "%.90s", v);
                    break;
                }
            }
            fclose(f);
        }
        choice_count++;
    }
    closedir(d);
    qsort(choices, (size_t)choice_count, sizeof(choices[0]), cmp_choice);
}

static void size_choices(void)
{
    choice_count = 0;
    for(int s = 0; s < UI_SIZE_COUNT && choice_count < MAX_CHOICES; s++) {
        if(!ui_size_offered((ui_size_t)s)) continue;
        snprintf(choices[choice_count].code, sizeof(choices[0].code), "%s", ui_size_name((ui_size_t)s));
        snprintf(choices[choice_count].name, sizeof(choices[0].name), "%s", ui_size_name((ui_size_t)s));
        choice_count++;
    }
}

static int choice_index(const char * code)
{
    for(int i = 0; i < choice_count; i++) if(strcmp(choices[i].code, code) == 0) return i;
    return 0;
}

/* --------------------------------------------------------- settings */

/* A view's option files: <what>-<view>-<screen kind> (the browser reads
 * the same names). */
static void view_key(const char * what, const char * view, char * out, size_t len)
{
    snprintf(out, len, "%s-%s-%s", what, view, ui_screen_kind_name());
}

static void sort_choices(void)
{
    choice_count = 2;
    snprintf(choices[0].code, sizeof(choices[0].code), "name");
    snprintf(choices[0].name, sizeof(choices[0].name), "%s", T(S_SORT_NAME));
    snprintf(choices[1].code, sizeof(choices[1].code), "recent");
    snprintf(choices[1].name, sizeof(choices[1].name), "%s", T(S_SORT_RECENT));
}

static void style_choices(void)
{
    choice_count = 1;
    snprintf(choices[0].code, sizeof(choices[0].code), "list");
    snprintf(choices[0].name, sizeof(choices[0].name), "%s", T(S_STYLE_LIST));
}

/* --------------------------------------------------------- the rows */

static row_t * add_row(row_kind_t kind, const char * label)
{
    if(row_count >= MAX_ROWS) return &rows[MAX_ROWS - 1];
    row_t * r = &rows[row_count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    snprintf(r->label, sizeof(r->label), "%s", label);
    return r;
}

static void add_page_row(const char * label, const char * to)
{
    row_t * r = add_row(ROW_PAGE, label);
    snprintf(r->page, sizeof(r->page), "%s", to);
}

static void add_toggle(const char * label, const char * key, bool def)
{
    row_t * r = add_row(ROW_TOGGLE, label);
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->def = def;
    bool on = config_read_bool(key, def);
    snprintf(r->value, sizeof(r->value), "%s", T(on ? S_SET_ON : S_SET_OFF));
}

static void add_choice(const char * label, const char * key, choice_kind_t ck, const char * current)
{
    row_t * r = add_row(ROW_CHOICE, label);
    snprintf(r->key, sizeof(r->key), "%s", key);
    r->choice = ck;
    r->index = choice_index(current);
    snprintf(r->value, sizeof(r->value), "%s", choice_count ? choices[r->index].name : current);
}

static void scan_row_value(row_t * r)
{
    long stamp;
    bool updating = false;
    mpd_library_state(&stamp, &updating);
    snprintf(r->value, sizeof(r->value), "%s", updating ? T(S_SET_SCANNING) : "");
}

static const char * group_title(const char * p)
{
    if(strcmp(p, "playback") == 0) return T(S_SET_PLAYBACK);
    if(strcmp(p, "library") == 0)  return T(S_SET_LIBRARY);
    if(strcmp(p, "display") == 0)  return T(S_SET_DISPLAY);
    if(strcmp(p, "controls") == 0) return T(S_SET_CONTROLS);
    if(strcmp(p, "system") == 0)   return T(S_SET_SYSTEM);
    return NULL;
}

static void build_rows(void)
{
    row_count = 0;
    if(strcmp(page, "hub") == 0) {
        add_page_row(T(S_SET_PLAYBACK), "playback");
        add_page_row(T(S_SET_LIBRARY), "library");
        add_page_row(T(S_SET_DISPLAY), "display");
        add_page_row(T(S_SET_CONTROLS), "controls");
        add_page_row(T(S_SET_SYSTEM), "system");
        add_row(ROW_TEXT, T(S_SET_VIEWS));
        for(int v = 0; v < HOME_VIEW_COUNT; v++) {
            if(v == HOME_NOW_PLAYING || v == HOME_SETTINGS) continue;
            add_page_row(home_view_name((home_view_t)v), home_view_id((home_view_t)v));
        }
        return;
    }
    if(strcmp(page, "playback") == 0) {
        add_toggle(T(S_SET_PLAY_THROUGH), "play_through", true);
    }
    else if(strcmp(page, "library") == 0) {
        add_toggle(T(S_SET_AUTOSCAN), "autoscan", true);
        add_toggle(T(S_SET_CACHE_ON_CARD), "cache_on_card", true);
        row_t * r = add_row(ROW_ACTION, T(S_SET_SCAN_NOW));
        r->action = ACT_SCAN;
        scan_row_value(r);
    }
    else if(strcmp(page, "display") == 0) {
        scan_choices("themes", ".theme", true);
        add_choice(T(S_SET_THEME), "theme", CH_THEME, config_theme_name());
        scan_choices("lang", ".txt", false);
        add_choice(T(S_SET_LANGUAGE), "language", CH_LANGUAGE, strings_language());
    }
    else if(strcmp(page, "controls") == 0) {
        add_row(ROW_TEXT, T(S_SET_EMPTY));
    }
    else if(strcmp(page, "system") == 0) {
        row_t * r = add_row(ROW_TEXT, T(S_SET_VERSION));
        snprintf(r->value, sizeof(r->value), "%s", SIMPLETON_VERSION);
    }
    else {
        /* a view's page: the picker's rows, remembered for this screen type */
        home_view_t v = home_view_from_id(page);
        if(v == HOME_FOLDERS) {
            char key[64], cur[32];
            view_key("style", page, key, sizeof(key));
            style_choices();
            add_choice(T(S_PICKER_STYLE), key, CH_STYLE, "list");
            view_key("size", page, key, sizeof(key));
            size_choices();
            config_read(key, cur, sizeof(cur), ui_size_name(UI_SIZE_DEFAULT));
            add_choice(T(S_PICKER_SIZE), key, CH_SIZE, cur);
            view_key("sort", page, key, sizeof(key));
            sort_choices();
            config_read(key, cur, sizeof(cur), "name");
            add_choice(T(S_PICKER_SORT), key, CH_SORT, cur);
            view_key("jump", page, key, sizeof(key));
            add_toggle(T(S_SET_JUMP_STRIP), key, true);
        }
        else add_row(ROW_TEXT, T(S_SET_EMPTY));
        add_row(ROW_HUB, T(S_SET_ALL));
    }
}

/* ------------------------------------------------------------ screen */

static void set_focus(int i)
{
    if(row_count == 0) return;
    if(i < 0) i = 0;
    if(i >= row_count) i = row_count - 1;
    focus = i;
    for(int k = 0; k < row_count; k++) {
        row_t * r = &rows[k];
        if(!r->obj) continue;
        bool on = (k == focus) && r->kind != ROW_TEXT;
        lv_obj_set_style_bg_opa(r->obj, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(r->obj, lv_color_hex(on ? UI_COLOR_FOCUS_FG : (r->kind == ROW_TEXT ? UI_COLOR_DIM : UI_COLOR_FG)), 0);
        if(r->val_label) lv_obj_set_style_text_color(r->val_label, lv_color_hex(on ? UI_COLOR_FOCUS_FG : UI_COLOR_DIM), 0);
    }
    if(rows[focus].obj) lv_obj_scroll_to_view(rows[focus].obj, LV_ANIM_OFF);
}

static void render(void)
{
    lv_obj_clean(list_box);
    const char * title = group_title(page);
    char buf[200];
    if(strcmp(page, "hub") == 0) title = T(S_SET_TITLE);
    else if(!title) {
        home_view_t v = home_view_from_id(page);
        ui_strf(buf, sizeof(buf), S_SET_VIEW_PAGE, "view", v < HOME_VIEW_COUNT ? home_view_name(v) : page, NULL);
        title = buf;
    }
    lv_label_set_text(header, title);

    for(int i = 0; i < row_count; i++) {
        row_t * r = &rows[i];
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lm.w, r->kind == ROW_TEXT && !r->value[0] ? lm.row_h * 3 / 4 : lm.row_h);
        lv_obj_set_style_radius(row, lm.row_radius, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
        lv_obj_set_style_pad_hor(row, lm.margin, 0);
        lv_obj_set_style_text_font(row, ui_font_px(r->kind == ROW_TEXT && !r->value[0] ? lm.chrome_font_px : lm.font_px), 0);
        lv_obj_t * l = lv_label_create(row);
        lv_label_set_text(l, r->label);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        /* one line tall, so a long label ends in "..." instead of wrapping */
        const lv_font_t * rf = lv_obj_get_style_text_font(row, 0);
        lv_obj_set_size(l, r->value[0] || r->kind == ROW_TOGGLE || r->kind == ROW_CHOICE ? lm.w * 65 / 100 : lm.w - 2 * lm.margin,
                        lv_font_get_line_height(rf));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        r->obj = row;
        r->val_label = NULL;
        if(r->kind == ROW_TOGGLE || r->kind == ROW_CHOICE || r->value[0]) {
            lv_obj_t * v = lv_label_create(row);
            char txt[120];
            if(r->kind == ROW_CHOICE) snprintf(txt, sizeof(txt), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, r->value);
            else snprintf(txt, sizeof(txt), "%s", r->value);
            lv_label_set_text(v, txt);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
            r->val_label = v;
        }
        if(r->kind == ROW_PAGE) {
            lv_obj_t * v = lv_label_create(row);
            lv_label_set_text(v, LV_SYMBOL_RIGHT);
            lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
            r->val_label = v;
        }
    }
    /* first selectable row */
    int f = 0;
    while(f < row_count && rows[f].kind == ROW_TEXT) f++;
    set_focus(f < row_count ? f : 0);

    char hints[300];
    snprintf(hints, sizeof(hints), "A %s  B %s  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " %s", T(S_HINT_SELECT), T(S_HINT_BACK), T(S_HINT_CHANGE));
    lv_label_set_text(footer, hints);
}

static void open_page(const char * p)
{
    snprintf(page, sizeof(page), "%s", p);
    build_rows();
    render();
}

/* --------------------------------------------------------- changes */

static void refresh_value(row_t * r)
{
    if(!r->val_label) return;
    char txt[120];
    if(r->kind == ROW_CHOICE) snprintf(txt, sizeof(txt), LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, r->value);
    else snprintf(txt, sizeof(txt), "%s", r->value);
    lv_label_set_text(r->val_label, txt);
}

/* A key of the form <what>-<view>-<kind> belongs to a view's page. */
static bool is_view_key(const char * key)
{
    return strncmp(key, "size-", 5) == 0 || strncmp(key, "sort-", 5) == 0 ||
           strncmp(key, "jump-", 5) == 0 || strncmp(key, "style-", 6) == 0;
}

static void flip_toggle(row_t * r)
{
    bool on = config_read_bool(r->key, r->def);
    on = !on;
    if(!config_write(r->key, on ? "1" : "0")) fprintf(stderr, "simpleton-ui: settings: cannot write %s\n", r->key);
    snprintf(r->value, sizeof(r->value), "%s", T(on ? S_SET_ON : S_SET_OFF));
    refresh_value(r);
    if(is_view_key(r->key) && view_changed_cb) view_changed_cb(page);
}

static void step_choice(row_t * r, int dir)
{
    if(choice_count == 0) return;
    /* the choice list on screen is the one built for this page; rebuild
     * it for this row's kind so two choice rows on a page can't mix */
    if(r->choice == CH_THEME) scan_choices("themes", ".theme", true);
    else if(r->choice == CH_LANGUAGE) scan_choices("lang", ".txt", false);
    else if(r->choice == CH_SIZE) size_choices();
    else if(r->choice == CH_SORT) sort_choices();
    else if(r->choice == CH_STYLE) style_choices();
    if(choice_count < 2) return;
    r->index = (r->index + dir + choice_count) % choice_count;
    const choice_t * c = &choices[r->index];
    if(!config_write(r->key, c->code)) { fprintf(stderr, "simpleton-ui: settings: cannot write %s\n", r->key); return; }
    snprintf(r->value, sizeof(r->value), "%s", c->name);
    refresh_value(r);
    fprintf(stderr, "simpleton-ui: settings: %s = %s\n", r->key, c->code);
    /* a view's option: the view takes it now; the theme and the language
     * are read at start-up: come straight back here with the new one */
    if(is_view_key(r->key)) { if(view_changed_cb) view_changed_cb(page); }
    else if(restart_cb) restart_cb(page);
}

static void run_action(row_t * r)
{
    if(r->action == ACT_SCAN) {
        if(mpd_update()) snprintf(r->value, sizeof(r->value), "%s", T(S_SET_SCANNING));
        refresh_value(r);
    }
}

/* ------------------------------------------------------------ public */

void settings_create(void (*restart)(const char *), void (*view_changed)(const char *))
{
    restart_cb = restart;
    view_changed_cb = view_changed;
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_t * stage = ui_stage_create(scr);
    ui_list_metrics(UI_SIZE_DEFAULT, &lm);

    header = lv_label_create(stage);
    lv_obj_set_size(header, lm.w, lm.header_h);
    lv_obj_set_pos(header, lm.x, 0);
    lv_obj_set_style_text_font(header, ui_font_px(lm.chrome_font_px), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_top(header, (lm.header_h - lm.chrome_font_px) / 2, 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);

    list_box = lv_obj_create(stage);
    lv_obj_remove_style_all(list_box);
    lv_obj_set_size(list_box, lm.w, lm.h);
    lv_obj_set_pos(list_box, lm.x, lm.y);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_box, lm.row_gap, 0);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_OFF);

    footer = lv_label_create(stage);
    lv_obj_set_size(footer, lm.w, lm.footer_h);
    lv_obj_set_pos(footer, lm.x, lm.y + lm.h);
    lv_obj_set_style_text_font(footer, ui_font_px(lm.chrome_font_px), 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_MODE_CLIP);
}

void settings_show(const char * p, bool fv)
{
    active = true;
    from_view = fv;
    hub_focus = 0;                   /* a fresh visit starts at the top */
    open_page(p && p[0] ? p : "hub");
    lv_screen_load(scr);
}

void settings_hide(void)
{
    active = false;
}

void settings_mpd_changed(void)
{
    if(!active) return;
    for(int i = 0; i < row_count; i++)
        if(rows[i].kind == ROW_ACTION && rows[i].action == ACT_SCAN) { scan_row_value(&rows[i]); refresh_value(&rows[i]); }
}

set_result_t settings_handle_action(ui_action_t a)
{
    row_t * r = row_count ? &rows[focus] : NULL;
    switch(a) {
        case ACT_UP:
        case ACT_DOWN: {
            int dir = a == ACT_DOWN ? 1 : -1;
            int i = focus;
            for(int tries = 0; tries < row_count; tries++) {
                i += dir;
                if(i < 0 || i >= row_count) { i = focus; break; }       /* stop at the ends, like the browser */
                if(rows[i].kind != ROW_TEXT) break;
            }
            set_focus(i);
            return SET_HANDLED;
        }
        case ACT_LEFT:
        case ACT_RIGHT:
            if(!r) return SET_HANDLED;
            if(r->kind == ROW_TOGGLE) flip_toggle(r);
            else if(r->kind == ROW_CHOICE) step_choice(r, a == ACT_RIGHT ? 1 : -1);
            return SET_HANDLED;
        case ACT_SELECT:
            if(!r) return SET_HANDLED;
            switch(r->kind) {
                case ROW_PAGE:   if(strcmp(page, "hub") == 0) hub_focus = focus; open_page(r->page); break;
                case ROW_TOGGLE: flip_toggle(r); break;
                case ROW_CHOICE: step_choice(r, 1); break;
                case ROW_ACTION: run_action(r); break;
                case ROW_HUB:    from_view = false; open_page("hub"); break;
                default: break;
            }
            return SET_HANDLED;
        case ACT_BACK:
            if(strcmp(page, "hub") == 0 || from_view) return SET_EXIT;
            open_page("hub");
            if(hub_focus > 0 && hub_focus < row_count && rows[hub_focus].kind != ROW_TEXT) set_focus(hub_focus);
            return SET_HANDLED;
        default:
            return SET_HANDLED;
    }
}
