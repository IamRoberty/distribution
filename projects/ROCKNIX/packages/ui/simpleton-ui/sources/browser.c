/*
 * SimpletonOS UI - folder browser screen (implementation). See browser.h.
 *
 * Layout (on the square stage; every measurement comes from
 * ui_list_metrics() - layout.h - for the list size in use):
 *   header  - current folder name, small dim text
 *   list    - one big row per entry, the focused row is unmistakable
 *   footer  - button hints
 *
 * All text the browser shows of its own comes from the string table
 * (strings.h); folder and track names are the library's and are shown as is.
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
#include "config.h"
#include "mpdc.h"
#include "picker.h"
#include "strings.h"
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
static lv_obj_t * stage;
static lv_obj_t * header;
static lv_obj_t * list;
static lv_obj_t * footer;

/* The list size in use and its measurements on this screen: the Folders
 * view's setting (remembered per screen type), or the picker's choice for
 * this run (0.14). */
static ui_list_t lm;

/* The view's options (Note 05 section 3). `view_opts` is what the picker
 * shows and what applies now; the remembered values live in the settings
 * files (size-folders-<kind> ...). */
enum { SORT_NAME, SORT_RECENT };
static picker_opts_t view_opts;      /* .strip is the per-view alphabet-picker switch */
#define strip_enabled (view_opts.strip)
static bool visited;                 /* the full picker has been shown on this screen type */
static bool suppress_overlay;        /* a load that must not open the strip (refresh, resume) */
static bool visited_before_open;     /* `visited` as the overlay opened (first-visit panel -> pill after) */
static void (*exit_cb)(void);        /* Back at the top level: leave the view */

#define STRIP_SCREENS 3              /* the strip appears past this many screenfuls... */
#define STRIP_MIN_LABELS 3           /* ...with at least this many distinct letters */

static nav_frame_t stack[MAX_DEPTH];
static int depth;                  /* number of frames above root */
static int exit_depth;             /* Back at this depth leaves the view: 0 normally, 1 when
                                    * opened straight at an album folder from a grid (0.16) */
static int root_focus;             /* remembered row at the root listing */
static mpd_listing_t cur;          /* listing currently on screen */
static mpd_entry_t * mpd_order;    /* cur.items as MPD listed them, so a sort can be undone */
static bool waiting_for_mpd;
static bool lib_scanning;            /* a library rescan is running (from MPD's notices) */
static void (*play_cb)(void);

static void view_key(const char * what, char * out, size_t len)
{
    snprintf(out, len, "%s-folders-%s", what, ui_screen_kind_name());
}

static ui_size_t read_size(void)
{
    char key[64], v[16];
    view_key("size", key, sizeof(key));
    config_read(key, v, sizeof(v), ui_size_name(UI_SIZE_DEFAULT));
    for(int i = 0; i < UI_SIZE_COUNT; i++)
        if(strcmp(ui_size_name((ui_size_t)i), v) == 0 && ui_size_offered((ui_size_t)i)) return (ui_size_t)i;
    return UI_SIZE_DEFAULT;
}

static int read_sort(void)
{
    char key[64], v[16];
    view_key("sort", key, sizeof(key));
    config_read(key, v, sizeof(v), "name");
    return strcmp(v, "recent") == 0 ? SORT_RECENT : SORT_NAME;
}

static void read_options(void)
{
    char key[64];
    view_opts.style = 0;
    view_opts.style_count = 1;
    view_opts.styles[0] = T(S_STYLE_LIST);
    view_opts.size = read_size();
    view_opts.sort = read_sort();
    view_opts.sort_count = 2;
    view_opts.sorts[SORT_NAME] = T(S_SORT_NAME);
    view_opts.sorts[SORT_RECENT] = T(S_SORT_RECENT);
    view_opts.jump = -1;
    view_opts.label_count = 0;
    view_key("jump", key, sizeof(key));
    view_opts.strip = config_read_bool(key, true);
    view_key("visited", key, sizeof(key));
    visited = config_read_bool(key, false);
}

static void write_options(void)
{
    char key[64];
    view_key("size", key, sizeof(key));
    config_write(key, ui_size_name(view_opts.size));
    view_key("sort", key, sizeof(key));
    config_write(key, view_opts.sort == SORT_RECENT ? "recent" : "name");
    view_key("jump", key, sizeof(key));
    config_write(key, view_opts.strip ? "1" : "0");
}

static void mark_visited(void)
{
    if(visited) return;
    visited = true;
    char key[64];
    view_key("visited", key, sizeof(key));
    config_write(key, "1");
}

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
static int focused_index(void);
static void close_overlay(void);

static void style_row(lv_obj_t * btn)
{
    lv_obj_set_height(btn, lm.row_h);
    lv_obj_set_style_radius(btn, lm.row_radius, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_FOCUS_KEY);   /* theme draws one here; the bg block is our focus cue */
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, lm.margin, 0);
    lv_obj_set_style_text_font(btn, ui_font_px(lm.font_px), 0);

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
    lv_obj_set_style_pad_hor(t, lm.margin, 0);
    lv_obj_set_user_data(t, (void *)(intptr_t)-1);
    return t;
}

static void show_message(const char * msg)
{
    clear_rows();
    lv_obj_t * t = add_text_row(msg, lm.row_h);
    lv_obj_set_style_text_font(t, ui_font_px(lm.font_px), 0);
}

static void set_header(void)
{
    const char * uri = cur_uri();
    const char * name = T(S_BROWSER_ROOT);
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

    if(cur.count == 0) { show_message(T(lib_scanning ? S_BROWSER_SCANNING : S_BROWSER_EMPTY)); return; }

    lv_obj_t * first = NULL, * want = NULL;
    for(int i = 0; i < cur.count; i++) {
        const mpd_entry_t * e = &cur.items[i];
        if(e->section) {
            lv_obj_t * d = add_text_row(e->section, lm.row_h / 2);
            lv_obj_set_style_text_font(d, ui_font_px(lm.chrome_font_px), 0);
            lv_obj_set_style_pad_top(d, lm.row_h * 3 / 20, 0);
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

/* "Recently changed": the folders of the listing newest first (a stable
 * sort of the folder entries, which MPD lists ahead of the files); the
 * tracks keep their order. "Name" is MPD's own order. */
static int by_mtime_desc(const void * a, const void * b)
{
    const mpd_entry_t * x = a, * y = b;
    return x->mtime > y->mtime ? -1 : x->mtime < y->mtime ? 1 : 0;
}

static void sort_listing(void)
{
    /* always from MPD's order: a re-sort of a sorted list can't undo itself */
    if(mpd_order && cur.count) memcpy(cur.items, mpd_order, sizeof(mpd_entry_t) * (size_t)cur.count);
    if(view_opts.sort != SORT_RECENT) return;
    int n = 0;
    while(n < cur.count && cur.items[n].kind == MPD_ENTRY_DIR) n++;
    if(n > 1) qsort(cur.items, (size_t)n, sizeof(mpd_entry_t), by_mtime_desc);
}

/* The jump labels of the listing on screen. */
static void build_labels(void)
{
    const char ** names = malloc(sizeof(char *) * (size_t)(cur.count ? cur.count : 1));
    if(!names) { view_opts.label_count = 0; return; }
    for(int i = 0; i < cur.count; i++) names[i] = cur.items[i].display;
    picker_labels_from_names(&view_opts, names, cur.count, false);
    free(names);
    view_opts.jump = view_opts.label_count ? 0 : -1;
}

static bool listing_is_long(void)
{
    return cur.count > STRIP_SCREENS * lm.rows && view_opts.label_count >= STRIP_MIN_LABELS;
}

/* Fetch `uri` into `cur` (sorted as the view asks); on MPD failure show a
 * waiting row and retry later. */
static bool load(const char * uri, int focus_idx)
{
    mpd_listing_t l;
    if(!mpd_lsinfo_expanded(uri, &l)) {
        set_header();
        show_message(T(mpd_is_connected() ? S_BROWSER_READ_FAILED : S_BROWSER_STARTING));
        waiting_for_mpd = !mpd_is_connected();
        return false;
    }
    waiting_for_mpd = false;
    mpd_listing_free(&cur);
    cur = l;
    free(mpd_order);
    mpd_order = cur.count ? malloc(sizeof(mpd_entry_t) * (size_t)cur.count) : NULL;
    if(mpd_order) memcpy(mpd_order, cur.items, sizeof(mpd_entry_t) * (size_t)cur.count);
    sort_listing();
    build_labels();
    close_overlay();                                     /* a new listing under an open strip: start clean */
    render(focus_idx);
    return true;
}

/* ---------- the picker and the strip ---------- */

static void open_strip(void);

/* While the strip is up it covers the list's last row: shorten the list
 * so the focused row always scrolls into view above it. */
static bool inset_on;
static void list_inset(bool on)
{
    /* the pill sits over the top row: the list starts below it */
    inset_on = on;
    lv_obj_set_height(list, on ? lm.h - lm.row_h : lm.h);
    lv_obj_set_y(list, on ? lm.y + lm.row_h : lm.y);
    lv_obj_update_layout(list);          /* so a scroll-into-view right after uses the new height */
}

static void close_overlay(void)
{
    if(picker_mode() != PICKER_NONE) picker_close();
    list_inset(false);
}

/* The strip's letter changed: the list follows. */
static void on_jump(int label, void * ctx)
{
    (void)ctx;
    for(int i = 0; i < cur.count; i++) {
        if(picker_label_of(&view_opts, cur.items[i].display) != label) continue;
        lv_obj_t * row = row_for(i);
        if(row) { lv_group_focus_obj(row); lv_obj_scroll_to_view(row, LV_ANIM_OFF); }
        return;
    }
}

static void apply_size(ui_size_t size);
static void go_up(void);

/* A value changed in the panel: apply it now and remember it. */
static void on_picker_change(const picker_opts_t * o, void * ctx)
{
    (void)ctx;
    bool resort = o->sort != view_opts.sort;
    bool resize = o->size != view_opts.size;
    view_opts.sort = o->sort;
    view_opts.style = o->style;
    view_opts.strip = o->strip;
    if(resize) apply_size(o->size);
    else if(resort && cur.count) {
        int keep = focused_index();
        char * uri = keep >= 0 && keep < cur.count ? strdup(cur.items[keep].uri) : NULL;
        sort_listing();
        render(0);
        if(uri) { focus_uri(uri); free(uri); }
    }
    write_options();
}

static void on_picker_close(bool back, void * ctx)
{
    picker_mode_t was = (picker_mode_t)(intptr_t)ctx;
    list_inset(false);
    mark_visited();
    /* Back on the pill leaves the view (Note 05: two presses from anywhere
     * in a long list); Back on the panel just closes it */
    if(back && picker_mode() == PICKER_NONE && was == PICKER_STRIP) { go_up(); return; }
    /* the first-visit panel closes onto a long list: the pill follows */
    if(was == PICKER_FULL && strip_enabled && listing_is_long() && !visited_before_open) open_strip();
}

static void open_strip(void)
{
    if(cur.count == 0) return;
    /* start the pill on the letter of the focused row */
    int i = focused_index();
    int l = (i >= 0 && i < cur.count) ? picker_label_of(&view_opts, cur.items[i].display) : -1;
    view_opts.jump = l >= 0 ? l : 0;
    list_inset(true);
    visited_before_open = visited;
    picker_open(PICKER_STRIP, &view_opts, on_jump, on_picker_change, on_picker_close, (void *)(intptr_t)PICKER_STRIP);
    lv_obj_t * row = row_for(i);
    if(row) lv_obj_scroll_to_view(row, LV_ANIM_OFF);
}

static void open_full(void)
{
    int i = focused_index();
    int l = (i >= 0 && i < cur.count) ? picker_label_of(&view_opts, cur.items[i].display) : -1;
    view_opts.jump = l >= 0 ? l : 0;
    visited_before_open = visited;
    picker_open(PICKER_FULL, &view_opts, on_jump, on_picker_change, on_picker_close, (void *)(intptr_t)PICKER_FULL);
}

/* After a listing is entered (going down, or the first one): the panel on
 * the first visit, else the pill over a long list. Not on the way back up:
 * the cursor is on the folder just left, and that is the place. */
static void after_enter(void)
{
    if(suppress_overlay) { suppress_overlay = false; return; }
    if(cur.count == 0) return;
    if(!visited) { open_full(); return; }
    if(strip_enabled && listing_is_long()) open_strip();
}

/* The Settings button on the view: the panel, or away again. */
void browser_toggle_picker(void)
{
    if(picker_mode() != PICKER_NONE) { picker_toggle_panel(); return; }
    if(cur.count == 0) return;
    open_full();
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
        return;
    }
    after_enter();
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
    int rows = lm.rows;
    int i = focused_index();
    int target = i + dir * rows;
    if(target < 0) target = 0;
    if(target > cur.count - 1) target = cur.count - 1;
    if(target == i) return;
    lv_obj_t * row = row_for(target);
    if(row) lv_group_focus_obj(row);
}

/* ---------- public ---------- */

void browser_create(lv_obj_t * scr, lv_group_t * group, void (*on_play)(void), void (*on_exit)(void), const char * resume_uri)
{
    grp = group;
    play_cb = on_play;
    exit_cb = on_exit;
    read_options();

    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* On a TV this is a centred square with plain background either side -
     * an interim layout until the TV browse screens are designed. */
    stage = ui_stage_create(scr);
    scr = stage;

    ui_list_metrics(view_opts.size, &lm);
    const lv_font_t * chrome_font = ui_font_px(lm.chrome_font_px);

    header = lv_label_create(scr);
    lv_obj_set_size(header, lm.w, lm.header_h);
    lv_obj_set_pos(header, lm.x, 0);
    lv_obj_set_style_text_font(header, chrome_font, 0);
    lv_obj_set_style_text_color(header, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_pad_top(header, (lm.header_h - lm.chrome_font_px) / 2, 0);
    lv_label_set_long_mode(header, LV_LABEL_LONG_MODE_DOTS);

    list = lv_list_create(scr);
    lv_obj_set_size(list, lm.w, lm.h);
    lv_obj_set_pos(list, lm.x, lm.y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, lm.row_gap, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    footer = lv_label_create(scr);
    lv_obj_set_size(footer, lm.w, lm.footer_h);
    lv_obj_set_pos(footer, lm.x, lm.y + lm.h);
    lv_obj_set_style_text_font(footer, chrome_font, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(UI_COLOR_DIM), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_MODE_CLIP);
    /* Button letters and icons are the device's, the words are the string
     * table's. (When the A/B swap setting arrives, the letters come from the
     * same table as the input mapping - settings-menu.md.) */
    char hints[400];
    snprintf(hints, sizeof(hints), "A %s  B %s  " LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " %s  Y %s  X %s  Start %s",
             T(S_HINT_SELECT), T(S_HINT_BACK), T(S_HINT_PAGE), T(S_HINT_PLAY), T(S_HINT_NEXT), T(S_HINT_NOW_PLAYING));
    lv_label_set_text(footer, hints);

    picker_attach(stage, &lm);

    /* Coming back after a display switch: rebuild the folder stack from the
     * path, so Back still walks up through it (landing on the folder we came
     * out of - focus -1 means "find it by name"). No picker or strip on a
     * resumed listing: the person was already in it. */
    suppress_overlay = true;
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
        if(load(cur_uri(), 0)) { suppress_overlay = false; return; }
        while(depth) { free(stack[--depth].uri); stack[depth].uri = NULL; }   /* gone: start at the top */
        root_focus = 0;
    }
    suppress_overlay = false;
    if(load("", 0)) after_enter();
}

/* A size change: re-measure everything on the stage and draw the listing
 * again at the new size, on the same row. */
static void apply_size(ui_size_t size)
{
    view_opts.size = size;
    int keep = focused_index();
    ui_list_metrics(size, &lm);
    const lv_font_t * chrome_font = ui_font_px(lm.chrome_font_px);
    lv_obj_set_size(header, lm.w, lm.header_h);
    lv_obj_set_pos(header, lm.x, 0);
    lv_obj_set_style_text_font(header, chrome_font, 0);
    lv_obj_set_style_pad_top(header, (lm.header_h - lm.chrome_font_px) / 2, 0);
    lv_obj_set_size(list, lm.w, lm.h);
    lv_obj_set_pos(list, lm.x, lm.y);
    list_inset(inset_on);
    lv_obj_set_style_pad_row(list, lm.row_gap, 0);
    lv_obj_set_size(footer, lm.w, lm.footer_h);
    lv_obj_set_pos(footer, lm.x, lm.y + lm.h);
    lv_obj_set_style_text_font(footer, chrome_font, 0);
    picker_relayout(&lm);
    if(cur.count) render(keep < 0 ? 0 : keep);
    else render(0);
}

void browser_reload_options(void)
{
    ui_size_t old_size = view_opts.size;
    int old_sort = view_opts.sort;
    read_options();
    if(view_opts.size != old_size) apply_size(view_opts.size);
    if(view_opts.sort != old_sort && cur.count) {
        int keep = focused_index();
        char * uri = keep >= 0 && keep < cur.count ? strdup(cur.items[keep].uri) : NULL;
        /* back to MPD's order first: a re-sort of a sorted list can't undo itself */
        if(load(cur_uri(), 0) && uri) focus_uri(uri);
        free(uri);
    }
    if(!strip_enabled && picker_mode() == PICKER_STRIP) close_overlay();
    if(picker_mode() == PICKER_FULL) picker_relayout(&lm);      /* the Settings page can't be up at the same time, but be safe */
}

bool browser_overlay_active(void)
{
    return picker_mode() != PICKER_NONE;
}

/* Back on screen after another view had it: the picker (one for every
 * browse view) comes back to this stage, closed. */
void browser_shown(void)
{
    picker_attach(stage, &lm);
    list_inset(false);
}

static void drop_stack(void)
{
    while(depth) { free(stack[--depth].uri); stack[depth].uri = NULL; }
}

/* Open straight at a folder (a grid's album): just that folder on the
 * stack, so Back leaves the view rather than climbing. No picker or pill:
 * the person chose the album, not a listing. */
bool browser_open_folder(const char * uri)
{
    close_overlay();
    drop_stack();
    stack[depth].uri = strdup(uri);
    stack[depth].focus = 0;
    depth = 1;
    exit_depth = 1;
    suppress_overlay = true;
    bool ok = load(uri, 0);
    if(!ok) { drop_stack(); exit_depth = 0; }       /* the caller stays where it was */
    suppress_overlay = false;
    return ok;
}

/* The Folders view from Home, after the browser was opened at an album:
 * back to the library root as the view's own entry. */
void browser_open_root(void)
{
    if(exit_depth == 0) return;
    close_overlay();
    drop_stack();
    exit_depth = 0;
    root_focus = 0;
    if(load("", 0)) after_enter();
}

bool browser_opened_at_folder(void) { return exit_depth != 0; }

const char * browser_current_uri(void) { return cur_uri(); }
bool browser_at_root(void) { return depth == 0; }

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

/* Up one folder, or out of the view at the top. */
static void go_up(void)
{
    if(depth <= exit_depth) { if(exit_cb) exit_cb(); return; }
    char * left = stack[--depth].uri;
    stack[depth].uri = NULL;
    int f = depth ? stack[depth - 1].focus : root_focus;
    if(load(cur_uri(), f < 0 ? 0 : f) && f < 0) focus_uri(left);
    free(left);
}

void browser_handle_action(ui_action_t a)
{
    if(picker_mode() != PICKER_NONE) { picker_handle_action(a); return; }
    switch(a) {
        case ACT_BACK:
            /* in a long list Back brings the strip first; with the strip up
             * (or in a short list) it leaves (Note 05 section 3) */
            if(strip_enabled && listing_is_long()) open_strip();
            else go_up();
            break;
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
