/*
 * SimpletonOS UI - folder browser screen (implementation). See browser.h.
 *
 * Layout (designed on the 720x720 panel, scaled to the stage - theme.h):
 *   header  - current folder name, small dim text
 *   list    - one big row per entry, the focused row is unmistakable
 *   footer  - button hints
 *
 * Focus rules (non-negotiable, from learnings.md): a row is always focused,
 * from the very first list, and the focused row is visibly different.
 *
 * 1 Oct 2026:
 *   - The list is the expanded folder (mpd_lsinfo_expanded): a SACD ISO, a
 *     multi-track DFF or a CUE album shows as its tracks in place, never as a
 *     file to open first.
 *   - Left/right page up/down by one screenful (and auto-repeat like
 *     up/down), for long lists from a slow TV remote.
 *
 * Row bookkeeping: every selectable row carries its index into `cur` as
 * user data; divider rows carry -1 and are not in the focus group, so the
 * list's children are no longer 1:1 with `cur` - look rows up by index.
 */
#include "browser.h"
#include "mpdc.h"
#include "theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32
#define DIVIDER_H PX(40)

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
static bool lib_scanning;            /* a library rescan is running (from MPD's notices) */
static void (*play_cb)(void);

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
static void focus_uri(const char * uri);

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

/* A non-interactive text row: messages (MPD not up yet, empty folder) and
 * the disc dividers. */
static lv_obj_t * add_text_row(const char * msg, int height)
{
    lv_obj_t * t = lv_list_add_text(list, msg);
    lv_obj_set_height(t, height);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_hor(t, UI_MARGIN, 0);
    lv_obj_set_user_data(t, (void *)(intptr_t)-1);
    return t;
}

static void show_message(const char * msg)
{
    clear_rows();
    lv_obj_t * t = add_text_row(msg, UI_ROW_H);
    lv_obj_set_style_text_font(t, UI_FONT_LIST, 0);
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

/* The selectable row for entry `index`, or NULL. */
static lv_obj_t * row_for(int index)
{
    uint32_t n = lv_obj_get_child_count(list);
    for(uint32_t c = 0; c < n; c++) {
        lv_obj_t * o = lv_obj_get_child(list, (int32_t)c);
        if((int)(intptr_t)lv_obj_get_user_data(o) == index) return o;
    }
    return NULL;
}

/* Render `cur` into the list and focus row `focus_idx`. */
static void render(int focus_idx)
{
    clear_rows();
    set_header();

    if(cur.count == 0) { show_message(lib_scanning ? "Scanning library\xE2\x80\xA6" : "Nothing here"); return; }

    lv_obj_t * first = NULL, * want = NULL;
    for(int i = 0; i < cur.count; i++) {
        const mpd_entry_t * e = &cur.items[i];
        if(e->section) {
            lv_obj_t * d = add_text_row(e->section, DIVIDER_H);
            lv_obj_set_style_text_font(d, UI_FONT_HINT, 0);
            lv_obj_set_style_pad_top(d, PX(12), 0);
        }
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
    if(!mpd_lsinfo_expanded(uri, &l)) {
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
     * chosen one. Folders in the same listing are skipped, not recursed.
     * The folder is the expanded one, so both discs of a two-disc folder
     * are queued, and play-through continues into the next folder. */
    char ** uris = calloc((size_t)cur.count, sizeof(char *));
    if(!uris) return;
    int n = 0, start = 0;
    for(int i = 0; i < cur.count; i++) {
        if(cur.items[i].kind != MPD_ENTRY_FILE) continue;
        if(i == index) start = n;
        uris[n++] = cur.items[i].uri;
    }
    bool started = n && mpd_play_uris(uris, n, start);
    free(uris);
    if(started && play_cb) play_cb();
}

static void row_click_cb(lv_event_t * e)
{
    lv_obj_t * btn = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(btn);
    if(i < 0 || i >= cur.count) return;
    const mpd_entry_t * entry = &cur.items[i];

    if(entry->kind == MPD_ENTRY_DIR) { push_and_enter(entry->uri); return; }

    /* Containers are expanded at load; this probe is the fallback for a
     * file MPD can expand but the expansion didn't recognise by name. */
    mpd_listing_t probe;
    if(mpd_lsinfo(entry->uri, &probe)) {
        bool is_container = probe.count > 0 && !(probe.count == 1 && strcmp(probe.items[0].uri, entry->uri) == 0);
        mpd_listing_free(&probe);
        if(is_container) { push_and_enter(entry->uri); return; }
    }
    play_from(i);
}

/* Move the cursor a screenful up (dir < 0) or down (dir > 0). */
static void page(int dir)
{
    if(cur.count == 0) return;
    int rows = (UI_BASE - UI_HEADER_H - UI_FOOTER_H) / (UI_ROW_H + PX(4));
    if(rows < 1) rows = 1;
    int i = focused_index();
    int target = i + dir * rows;
    if(target < 0) target = 0;
    if(target > cur.count - 1) target = cur.count - 1;
    if(target == i) return;
    lv_obj_t * row = row_for(target);
    if(row) lv_group_focus_obj(row);
}

/* ---------- public ---------- */

void browser_create(lv_obj_t * scr, lv_group_t * group, void (*on_play)(void), const char * resume_uri)
{
    grp = group;
    play_cb = on_play;

    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* On a TV this is a centred square with plain background either side -
     * an interim layout until the TV browse screens are designed. */
    lv_obj_t * stage = ui_stage_create(scr);
    scr = stage;

    header = lv_label_create(scr);
    lv_obj_set_size(header, UI_BASE - 2 * UI_MARGIN, UI_HEADER_H);
    lv_obj_set_pos(header, UI_MARGIN, 0);
    lv_obj_set_style_text_font(header, UI_FONT_HEADER, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_top(header, (UI_HEADER_H - PX(20)) / 2, 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);

    list = lv_list_create(scr);
    lv_obj_set_size(list, UI_BASE - 2 * UI_MARGIN, UI_BASE - UI_HEADER_H - UI_FOOTER_H);
    lv_obj_set_pos(list, UI_MARGIN, UI_HEADER_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, PX(4), 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    footer = lv_label_create(scr);
    lv_obj_set_size(footer, UI_BASE - 2 * UI_MARGIN, UI_FOOTER_H);
    lv_obj_set_pos(footer, UI_MARGIN, UI_BASE - UI_FOOTER_H);
    lv_obj_set_style_text_font(footer, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(footer, "A select  B back  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " page  Y play  X next  Start now playing");

    /* Coming back after a display switch: rebuild the folder stack from the
     * path, so Back still walks up through it (landing on the folder we came
     * out of - focus -1 means "find it by name"). */
    if(resume_uri && resume_uri[0]) {
        char * path = strdup(resume_uri);
        for(char * slash = path; path && depth < MAX_DEPTH; ) {
            slash = strchr(slash, '/');
            if(slash) *slash = 0;
            stack[depth].uri = strdup(path);
            stack[depth].focus = -1;
            depth++;
            if(!slash) break;
            *slash++ = '/';
        }
        free(path);
        root_focus = -1;
        if(load(cur_uri(), 0)) return;
        while(depth) { free(stack[--depth].uri); stack[depth].uri = NULL; }   /* gone: start at the top */
        root_focus = 0;
    }
    load("", 0);
}

const char * browser_current_uri(void) { return cur_uri(); }

/* Focus the row whose uri is `uri`, if it's in the listing on screen. */
static void focus_uri(const char * uri)
{
    for(int i = 0; i < cur.count; i++) {
        if(strcmp(cur.items[i].uri, uri) != 0) continue;
        lv_obj_t * row = row_for(i);
        if(row) lv_group_focus_obj(row);
        return;
    }
}

void browser_handle_action(ui_action_t a)
{
    switch(a) {
        case ACT_BACK: {
            if(depth == 0) return;
            char * left = stack[--depth].uri;
            stack[depth].uri = NULL;
            int f = depth ? stack[depth - 1].focus : root_focus;
            if(load(cur_uri(), f < 0 ? 0 : f) && f < 0) focus_uri(left);
            free(left);
            break;
        }
        case ACT_LEFT:  page(-1); break;
        case ACT_RIGHT: page(+1); break;
        case ACT_MENU:
            break;   /* object menu: later */
        default:
            break;
    }
}

/* Reload the folder on screen, keeping the focused row (by name, so a row
 * added above it doesn't move the cursor). If the folder itself is gone
 * (card pulled), climb to the nearest parent that still exists. */
static void refresh(void)
{
    lv_obj_t * f = lv_group_get_focused(grp);
    int i = f ? (int)(intptr_t)lv_obj_get_user_data(f) : -1;
    char * keep = (i >= 0 && i < cur.count) ? strdup(cur.items[i].uri) : NULL;
    int idx = i < 0 ? 0 : i;

    while(!load(cur_uri(), idx) && depth > 0 && mpd_is_connected()) {
        free(stack[--depth].uri);
        stack[depth].uri = NULL;
        idx = 0;
    }
    if(keep) { focus_uri(keep); free(keep); }
}

void browser_tick(void)
{
    if(waiting_for_mpd) load(cur_uri(), 0);
}

void browser_library_changed(bool db_changed)
{
    /* One status query per notice (MPD only says *that* the update state
     * changed, not which way): is a rescan running now? */
    long stamp;
    bool scanning = false, was = lib_scanning;
    if(mpd_library_state(&stamp, &scanning)) lib_scanning = scanning;
    /* reload when the library changed, or when an empty list should switch
     * between "Scanning library..." and "Nothing here" */
    if(db_changed || (was != lib_scanning && cur.count == 0)) refresh();
}
