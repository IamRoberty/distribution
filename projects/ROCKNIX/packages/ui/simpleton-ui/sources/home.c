/*
 * SimpletonOS UI - the Home page (implementation). See home.h.
 *
 * Two renderings of the same entries:
 *   - the theme's tape shelf (shelf.c) when the .theme says "home shelf"
 *     and at least one tile could be read;
 *   - a plain list on the stage otherwise (the shared list look, so a theme
 *     with no Home art still has a Home page).
 * Both keep the selection in `sel`, so switching rendering is a detail.
 *
 * The Now Playing entry is part of the list from the start (its spine is
 * composed with the others) and shown or hidden as playback starts and
 * stops, so it costs nothing to appear.
 */
#include "home.h"
#include "config.h"
#include "mpdc.h"
#include "shelf.h"
#include "strings.h"
#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char * id; str_id_t name; } views[HOME_VIEW_COUNT] = {
    [HOME_FOLDERS]       = { "folders",       S_HOME_FOLDERS },
    [HOME_ALBUMS]        = { "albums",        S_HOME_ALBUMS },
    [HOME_ARTISTS]       = { "artists",       S_HOME_ARTISTS },
    [HOME_TRACKS]        = { "tracks",        S_HOME_TRACKS },
    [HOME_RECENT]        = { "recent",        S_HOME_RECENT },
    [HOME_FAVORITES]     = { "favorites",     S_HOME_FAVORITES },
    [HOME_PLAYLISTS]     = { "playlists",     S_HOME_PLAYLISTS },
    [HOME_COLLECTIONS]   = { "collections",   S_HOME_COLLECTIONS },
    [HOME_VISUALIZATION] = { "visualization", S_HOME_VISUALIZATION },
    [HOME_NOW_PLAYING]   = { "nowplaying",    S_HOME_NOW_PLAYING },
    [HOME_SETTINGS]      = { "settings",      S_HOME_SETTINGS },
};

/* The page's entries in the order shown: the theme's tape order, or the
 * table's order for the plain list. */
typedef struct {
    home_view_t  view;
    str_id_t     name;        /* the word on the tape: the view's, or the theme's alias ("mixtapes") */
    shelf_item_t item;        /* the theme's text rule (shelf only)                                   */
} entry_t;

#define MAX_ENTRIES 24

static lv_obj_t * scr;
static lv_obj_t * shelf;          /* NULL = plain list */
static lv_obj_t * list_rows[MAX_ENTRIES];
static lv_obj_t * list_box;
static entry_t entries[MAX_ENTRIES];
static int count;
static int sel;
static bool playing;              /* something is playing or paused */
static bool active;
static void (*open_cb)(home_view_t);

/* ------------------------------------------------------------ names */

const char * home_view_id(home_view_t v)   { return v < HOME_VIEW_COUNT ? views[v].id : ""; }
const char * home_view_name(home_view_t v) { return v < HOME_VIEW_COUNT ? T(views[v].name) : ""; }

home_view_t home_view_from_id(const char * id)
{
    if(strcmp(id, "mixtapes") == 0) return HOME_PLAYLISTS;
    for(int v = 0; v < HOME_VIEW_COUNT; v++) if(strcmp(views[v].id, id) == 0) return (home_view_t)v;
    return HOME_VIEW_COUNT;
}

/* ------------------------------------------------------- theme file */

static bool parse_hex(const char * s, uint32_t * out)
{
    if(*s == '#') s++;
    if(strlen(s) != 6) return false;
    char * end;
    *out = (uint32_t)strtoul(s, &end, 16);
    return *end == 0;
}

/* Read the Home lines of the theme. Fills `st` and the entries; returns
 * true when the theme draws Home as a shelf with at least one tape. */
static bool read_theme(shelf_style_t * st)
{
    shelf_style_defaults(st);
    char path[512];
    config_theme_file(path, sizeof(path));
    FILE * f = fopen(path, "r");
    if(!f) return false;

    bool is_shelf = false;
    count = 0;
    char line[512];
    while(fgets(line, sizeof(line), f)) {
        char a[64], b[128], c[128], d[32], e[32], g[32], al[32];
        int n;
        if(sscanf(line, "home %63s", a) == 1) { is_shelf = strcmp(a, "shelf") == 0; continue; }
        if(sscanf(line, "shelf %63s %127s", a, b) == 2) {
            uint32_t rgb;
            if(strcmp(a, "rest") == 0)        st->rest = atof(b);
            else if(strcmp(a, "pull") == 0)   st->pull = atof(b);
            else if(strcmp(a, "rail") == 0)   st->rail = atoi(b);
            else if(strcmp(a, "shadow") == 0) st->shadow = atoi(b);
            else if(strcmp(a, "wood-h") == 0) snprintf(st->wood_h, sizeof(st->wood_h), "%s", b);
            else if(strcmp(a, "wood-v") == 0) snprintf(st->wood_v, sizeof(st->wood_v), "%s", b);
            else if(strcmp(a, "back") == 0 && parse_hex(b, &rgb)) st->back = rgb;
            continue;
        }
        n = sscanf(line, "tape %63s %127s %127s %31s %31s %31s %31s", a, b, c, d, e, g, al);
        if(n >= 6 && count < MAX_ENTRIES) {
            home_view_t v = home_view_from_id(a);
            if(v == HOME_VIEW_COUNT) { fprintf(stderr, "simpleton-ui: home: unknown tape \"%s\" in %s\n", a, path); continue; }
            for(int i = 0; i < count; i++) if(entries[i].view == v) { v = HOME_VIEW_COUNT; break; }
            if(v == HOME_VIEW_COUNT) continue;              /* listed twice: keep the first */
            entry_t * en = &entries[count];
            memset(en, 0, sizeof(*en));
            en->view = v;
            en->name = strcmp(a, "mixtapes") == 0 ? S_HOME_MIXTAPES : views[v].name;
            snprintf(en->item.tile, sizeof(en->item.tile), "%s", b);
            snprintf(en->item.font, sizeof(en->item.font), "%s", c);
            en->item.size_pt = atoi(d);
            if(en->item.size_pt < 8) en->item.size_pt = 60;
            en->item.letter_case = strcmp(e, "upper") == 0 ? SHELF_CASE_UPPER
                                 : strcmp(e, "lower") == 0 ? SHELF_CASE_LOWER
                                 : strcmp(e, "title") == 0 ? SHELF_CASE_TITLE : SHELF_CASE_AS_IS;
            uint32_t ink;
            en->item.ink = parse_hex(g, &ink) ? ink : 0x000000;
            en->item.align = SHELF_ALIGN_CENTRE;
            if(n >= 7) {
                if(strcmp(al, "start") == 0)    en->item.align = SHELF_ALIGN_START;
                else if(strcmp(al, "end") == 0) en->item.align = SHELF_ALIGN_END;
            }
            count++;
        }
    }
    fclose(f);
    if(st->rest <= 0.1 || st->rest > 1.0) st->rest = 0.889;
    if(st->pull <= st->rest || st->pull > 1.0) st->pull = 1.0;
    return is_shelf && count > 0;
}

/* The plain list's entries: the table's order, Now Playing after Visualization. */
static void default_entries(void)
{
    count = 0;
    for(int v = 0; v < HOME_VIEW_COUNT && count < MAX_ENTRIES; v++) {
        entries[count].view = (home_view_t)v;
        entries[count].name = views[v].name;
        count++;
    }
}

/* ---------------------------------------------------------- the list */

static void list_focus(int i)
{
    for(int k = 0; k < count; k++) {
        lv_obj_t * row = list_rows[k];
        if(!row) continue;
        bool on = (k == i);
        lv_obj_set_style_bg_opa(row, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row, lv_color_hex(on ? UI_COLOR_FOCUS_FG : UI_COLOR_FG), 0);
    }
    if(list_rows[i]) lv_obj_scroll_to_view(list_rows[i], LV_ANIM_OFF);
}

static void build_list(void)
{
    ui_list_t lm;
    ui_list_metrics(UI_SIZE_DEFAULT, &lm);
    lv_obj_t * stage = ui_stage_create(scr);

    lv_obj_t * header = lv_label_create(stage);
    lv_obj_set_size(header, lm.w, lm.header_h);
    lv_obj_set_pos(header, lm.x, 0);
    lv_obj_set_style_text_font(header, ui_font_px(lm.chrome_font_px), 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_top(header, (lm.header_h - lm.chrome_font_px) / 2, 0);
    lv_label_set_text(header, T(S_HOME_TITLE));

    list_box = lv_obj_create(stage);
    lv_obj_remove_style_all(list_box);
    lv_obj_set_size(list_box, lm.w, lm.h);
    lv_obj_set_pos(list_box, lm.x, lm.y);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_box, lm.row_gap, 0);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_OFF);

    for(int i = 0; i < count; i++) {
        lv_obj_t * row = lv_obj_create(list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lm.w, lm.row_h);
        lv_obj_set_style_radius(row, lm.row_radius, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
        lv_obj_set_style_pad_hor(row, lm.margin, 0);
        lv_obj_set_style_text_font(row, ui_font_px(lm.font_px), 0);
        lv_obj_t * l = lv_label_create(row);
        lv_label_set_text(l, T(entries[i].name));
        lv_obj_set_size(l, lm.w - 2 * lm.margin, lv_font_get_line_height(ui_font_px(lm.font_px)));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
        list_rows[i] = row;
        if(entries[i].view == HOME_NOW_PLAYING && !playing) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t * footer = lv_label_create(stage);
    lv_obj_set_size(footer, lm.w, lm.footer_h);
    lv_obj_set_pos(footer, lm.x, lm.y + lm.h);
    lv_obj_set_style_text_font(footer, ui_font_px(lm.chrome_font_px), 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_MODE_CLIP);
    char hints[200];
    snprintf(hints, sizeof(hints), "A %s  Y %s  X %s  Start %s", T(S_HINT_OPEN), T(S_HINT_PLAY), T(S_HINT_NEXT), T(S_HINT_NOW_PLAYING));
    lv_label_set_text(footer, hints);
    list_focus(sel);
}

/* ------------------------------------------------------------ public */

static int entry_of(home_view_t v)
{
    for(int i = 0; i < count; i++) if(entries[i].view == v) return i;
    return -1;
}

static void refresh_playing(void)
{
    mpd_status_t st;
    bool now = mpd_status(&st) && (strcmp(st.state, "play") == 0 || strcmp(st.state, "pause") == 0);
    if(now == playing) return;
    playing = now;
    int i = entry_of(HOME_NOW_PLAYING);
    if(i < 0) return;
    if(shelf) {
        shelf_set_visible(shelf, i, playing);
        sel = shelf_selected(shelf);
    }
    else if(list_rows[i]) {
        if(playing) lv_obj_remove_flag(list_rows[i], LV_OBJ_FLAG_HIDDEN);
        else {
            lv_obj_add_flag(list_rows[i], LV_OBJ_FLAG_HIDDEN);
            if(sel == i) { sel = (i + 1) % count; list_focus(sel); }
        }
    }
}

void home_create(void (*on_open)(home_view_t))
{
    open_cb = on_open;
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    mpd_status_t st;
    playing = mpd_status(&st) && (strcmp(st.state, "play") == 0 || strcmp(st.state, "pause") == 0);

    shelf_style_t style;
    bool want_shelf = read_theme(&style);
    if(want_shelf) {
        shelf_item_t items[MAX_ENTRIES];
        for(int i = 0; i < count; i++) {
            items[i] = entries[i].item;
            items[i].label = T(entries[i].name);
        }
        shelf = shelf_create(scr, &style);
        if(shelf && !shelf_set_items(shelf, items, count, 0)) {
            lv_obj_delete(shelf);
            shelf = NULL;
            fprintf(stderr, "simpleton-ui: home: no usable tape art for theme %s, plain list\n", config_theme_name());
        }
    }
    if(shelf) {
        int np = entry_of(HOME_NOW_PLAYING);
        if(np >= 0 && !playing) shelf_set_visible(shelf, np, false);
        sel = shelf_selected(shelf);
    }
    else {
        default_entries();
        sel = 0;
        build_list();
    }
    fprintf(stderr, "simpleton-ui: home: %d entries, %s\n", count, shelf ? "tape shelf" : "plain list");
}

void home_show(void)
{
    active = true;
    refresh_playing();
    lv_screen_load(scr);
}

void home_hide(void)
{
    active = false;
}

void home_mpd_changed(void)
{
    if(active) refresh_playing();
}

void home_handle_action(ui_action_t a)
{
    if(shelf) {
        if(shelf_handle_action(shelf, a)) { sel = shelf_selected(shelf); return; }
        if(a == ACT_SELECT && open_cb) open_cb(entries[sel].view);
        return;
    }
    switch(a) {
        case ACT_UP:
        case ACT_DOWN: {
            int dir = a == ACT_DOWN ? 1 : -1;
            int i = sel;
            for(int tries = 0; tries < count; tries++) {
                i = (i + dir + count) % count;
                if(!(entries[i].view == HOME_NOW_PLAYING && !playing)) break;
            }
            sel = i;
            list_focus(sel);
            break;
        }
        case ACT_SELECT:
            if(open_cb) open_cb(entries[sel].view);
            break;
        default:
            break;
    }
}
