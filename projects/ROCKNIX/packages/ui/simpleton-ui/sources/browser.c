/*
 * SimpletonOS UI - folder browser screen (implementation). See browser.h.
 *
 * Layout (720x720 profile):
 *   header  - current folder name, small dim text
 *   list    - one big row per entry, the focused row is unmistakable
 *   footer  - button hints
 *
 * Focus rules (non-negotiable, from learnings.md): a row is always focused,
 * from the very first list, and the focused row is visibly different.
 */
#include "browser.h"
#include "mpdc.h"
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32

typedef struct {
    char * uri;
    int    focus;      /* row index to restore when we come back */
} nav_frame_t;

static lv_group_t * grp;
static lv_obj_t * header;
static lv_obj_t * list;
static lv_obj_t * footer;

static nav_frame_t stack[MAX_DEPTH];
static int depth;                  /* number of frames above root */
static int root_focus;             /* remembered row at the root listing */
static mpd_listing_t cur;          /* listing currently on screen */
static bool waiting_for_mpd;

static const char * cur_uri(void) { return depth ? stack[depth - 1].uri : ""; }

/* ---------- rows ---------- */

static void row_focus_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    if(!label) return;
    lv_label_set_long_mode(label, lv_event_get_code(e) == LV_EVENT_FOCUSED
                                  ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                                  : LV_LABEL_LONG_MODE_CLIP);
}

static void row_click_cb(lv_event_t * e);

static void style_row(lv_obj_t * btn)
{
    lv_obj_set_height(btn, UI_ROW_H);
    lv_obj_set_style_radius(btn, UI_ROW_RADIUS, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);   /* theme draws one here; the bg block is our focus cue */
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, UI_MARGIN, 0);
    lv_obj_set_style_text_font(btn, UI_FONT_LIST, 0);

    /* unfocused: no background; focused: solid accent block + white text */
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_FG), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_FOCUS_BG), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(UI_COLOR_FOCUS_FG), LV_STATE_FOCUSED);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(btn, row_focus_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(btn, row_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * label = lv_obj_get_child(btn, 0);
    if(label) lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
}

static void clear_rows(void)
{
    lv_group_remove_all_objs(grp);
    lv_obj_clean(list);
}

/* A non-interactive message row (MPD not up yet, empty folder). */
static void show_message(const char * msg)
{
    clear_rows();
    lv_obj_t * t = lv_list_add_text(list, msg);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(t, UI_FONT_LIST, 0);
    lv_obj_set_style_pad_hor(t, UI_MARGIN, 0);
}

static void set_header(void)
{
    const char * uri = cur_uri();
    const char * name = "Music";
    if(uri[0]) {
        const char * slash = strrchr(uri, '/');
        name = slash ? slash + 1 : uri;
    }
    lv_label_set_text(header, name);
}

/* Render `cur` into the list and focus row `focus_idx`. */
static void render(int focus_idx)
{
    clear_rows();
    set_header();

    if(cur.count == 0) { show_message("Nothing here"); return; }

    lv_obj_t * first = NULL, * want = NULL;
    for(int i = 0; i < cur.count; i++) {
        const mpd_entry_t * e = &cur.items[i];
        char text[600];
        /* folders get a trailing slash rather than an icon: unambiguous, no assets */
        snprintf(text, sizeof(text), e->kind == MPD_ENTRY_DIR ? "%s/" : "%s", e->display);
        lv_obj_t * btn = lv_list_add_button(list, NULL, text);
        style_row(btn);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_group_add_obj(grp, btn);
        if(i == 0) first = btn;
        if(i == focus_idx) want = btn;
    }
    lv_group_focus_obj(want ? want : first);
}

/* Fetch `uri` into `cur`; on MPD failure show a waiting row and retry later. */
static bool load(const char * uri, int focus_idx)
{
    mpd_listing_t l;
    if(!mpd_lsinfo(uri, &l)) {
        set_header();
        show_message(mpd_is_connected() ? "Couldn't read this folder" : "Starting library\xE2\x80\xA6");
        waiting_for_mpd = !mpd_is_connected();
        return false;
    }
    waiting_for_mpd = false;
    mpd_listing_free(&cur);
    cur = l;
    render(focus_idx);
    return true;
}

static int focused_index(void)
{
    lv_obj_t * f = lv_group_get_focused(grp);
    return f ? (int)(intptr_t)lv_obj_get_user_data(f) : 0;
}

static void push_and_enter(const char * uri)
{
    if(depth >= MAX_DEPTH) return;
    /* remember where we were, so Back lands on the same row */
    int remembered = focused_index();
    if(depth) stack[depth - 1].focus = remembered;
    else      root_focus = remembered;

    stack[depth].uri = strdup(uri);
    stack[depth].focus = 0;
    depth++;
    if(!load(uri, 0)) {
        /* couldn't enter: unwind so Back doesn't go somewhere we never were */
        free(stack[--depth].uri);
        stack[depth].uri = NULL;
        load(cur_uri(), remembered);
    }
}

static void play_from(int index)
{
    /* Queue every playable entry in this folder, in order, and start at the
     * chosen one. Folders in the same listing are skipped, not recursed. */
    char ** uris = calloc((size_t)cur.count, sizeof(char *));
    if(!uris) return;
    int n = 0, start = 0;
    for(int i = 0; i < cur.count; i++) {
        if(cur.items[i].kind != MPD_ENTRY_FILE) continue;
        if(i == index) start = n;
        uris[n++] = cur.items[i].uri;
    }
    if(n) mpd_play_uris(uris, n, start);
    free(uris);
}

static void row_click_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    if(i < 0 || i >= cur.count) return;
    const mpd_entry_t * entry = &cur.items[i];

    if(entry->kind == MPD_ENTRY_DIR) { push_and_enter(entry->uri); return; }

    /* A file might be a container MPD can expand (SACD ISO, CUE). Ask MPD:
     * if lsinfo on it yields entries other than itself, step into it. */
    mpd_listing_t probe;
    if(mpd_lsinfo(entry->uri, &probe)) {
        bool is_container = probe.count > 0 && !(probe.count == 1 && strcmp(probe.items[0].uri, entry->uri) == 0);
        mpd_listing_free(&probe);
        if(is_container) { push_and_enter(entry->uri); return; }
    }
    play_from(i);
}

/* ---------- public ---------- */

void browser_create(lv_obj_t * scr, lv_group_t * group)
{
    grp = group;

    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    header = lv_label_create(scr);
    lv_obj_set_size(header, UI_BASE - 2 * UI_MARGIN, UI_HEADER_H);
    lv_obj_set_pos(header, UI_MARGIN, 0);
    lv_obj_set_style_text_font(header, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_top(header, (UI_HEADER_H - 20) / 2, 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);

    list = lv_list_create(scr);
    lv_obj_set_size(list, UI_BASE - 2 * UI_MARGIN, UI_BASE - UI_HEADER_H - UI_FOOTER_H);
    lv_obj_set_pos(list, UI_MARGIN, UI_HEADER_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    footer = lv_label_create(scr);
    lv_obj_set_size(footer, UI_BASE - 2 * UI_MARGIN, UI_FOOTER_H);
    lv_obj_set_pos(footer, UI_MARGIN, UI_BASE - UI_FOOTER_H);
    lv_obj_set_style_text_font(footer, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_text(footer, "A select    B back    Y play/pause    X next");

    load("", 0);
}

void browser_handle_action(ui_action_t a)
{
    switch(a) {
        case ACT_BACK:
            if(depth == 0) return;
            free(stack[--depth].uri);
            stack[depth].uri = NULL;
            load(cur_uri(), depth ? stack[depth - 1].focus : root_focus);
            break;
        case ACT_MENU:
            break;   /* object menu: later */
        default:
            break;
    }
}

void browser_tick(void)
{
    if(waiting_for_mpd) load(cur_uri(), 0);
}
