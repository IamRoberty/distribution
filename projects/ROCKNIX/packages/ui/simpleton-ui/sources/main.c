/*
 * SimpletonOS UI - entry point.
 *
 * Wiring: DRM display -> LVGL; joypad (evdev) -> abstract actions -> either
 * the LVGL keypad path (navigation, select) or app-level handlers (back,
 * transport). MPD is reached through mpdc.c. The UI must come up even when
 * MPD isn't running yet, and keeps retrying it in the background.
 *
 * Display (29 Sep 2026): display.c picks the output (HDMI if connected,
 * else the panel, 1080p max on a TV); layout_init() then records the real
 * screen and the square stage, and every size is worked out from those. When HDMI is plugged or unplugged the UI
 * restarts itself on the new output (a fresh exec, well under a second;
 * playback is MPD's and never stops), carrying over which screen and which
 * folder was showing: --folder <uri>, --nowplaying, --settings <page>.
 * Settings that are read at start-up (theme, language, a view's size) are
 * applied with the same restart (0.13).
 *
 * Keypad notes: LVGL's keypad driver moves group focus only on LV_KEY_NEXT /
 * LV_KEY_PREV, so D-pad down/up are mapped to those; ENTER produces the
 * PRESSED/CLICKED pair on the focused row. Left/right in the browser page
 * the list (1 Oct 2026) and go to the browser directly. Everything else
 * bypasses LVGL.
 *
 * MPD notices (1 Oct 2026): a third MPD connection waits in `idle`; its fd is
 * in the poll() set, so when MPD reports a change the loop wakes and hands
 * it to the browser (library), the now-playing screen (playback) and
 * play-through (queue). Nothing polls MPD on a timer any more; the 2 s timer
 * only reconnects after an MPD restart, and treats a reconnect as
 * "everything changed".
 *
 * Screens (0.13, Note 05 step 4): the Home page is the root; it opens the
 * folder browser, now-playing, Settings, or a notice for a view that isn't
 * built yet. Back from a view's top level returns to Home; Start jumps to
 * now-playing from anywhere and Back there returns to where it came from;
 * Select (the settings button) opens the settings page of the view on
 * screen. The browser lives on the default screen and uses the keypad
 * group; the other screens are their own lv_obj screens and take every
 * action directly. main.c owns the switches.
 */

#include "lvgl.h"
#include "browser.h"
#include "cec.h"
#include "config.h"
#include "curation.h"
#include "dacpower.h"
#include "keyboard.h"
#include "objmenu.h"
#include "display.h"
#include "fonts.h"
#include "grid.h"
#include "albumpage.h"
#include "artists.h"
#include "hdmisilence.h"
#include "history.h"
#include "home.h"
#include "input.h"
#include "layout.h"
#include "libs.h"
#include "listpage.h"
#include "mpdc.h"
#include "notice.h"
#include "nowplaying.h"
#include "picker.h"
#include "playthrough.h"
#include "power.h"
#include "rows.h"
#include "settings.h"
#include "strings.h"
#include "theme.h"

#include <execinfo.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* ---- keypad bridge: tiny queue of (key, pressed) pairs for the indev ---- */

#define KQ_LEN 32
static struct { uint32_t key; bool pressed; } kq[KQ_LEN];
static int kq_head, kq_tail;

static void key_tap(uint32_t key)
{
    /* one press + one release; LVGL needs both to register a click */
    for(int i = 0; i < 2; i++) {
        int next = (kq_tail + 1) % KQ_LEN;
        if(next == kq_head) return;
        kq[kq_tail].key = key;
        kq[kq_tail].pressed = (i == 0);
        kq_tail = next;
    }
}

static void keypad_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    (void)indev;
    if(kq_head == kq_tail) { data->state = LV_INDEV_STATE_RELEASED; return; }
    data->key = kq[kq_head].key;
    data->state = kq[kq_head].pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    kq_head = (kq_head + 1) % KQ_LEN;
    data->continue_reading = (kq_head != kq_tail);
}

/* ---- screens ---- */

typedef enum { SCR_HOME, SCR_BROWSER, SCR_NOWPLAYING, SCR_SETTINGS, SCR_NOTICE, SCR_ALBUMS, SCR_ALBUMPAGE, SCR_LIST, SCR_ROWS } screen_t;

static lv_obj_t * browser_scr;
static screen_t cur = SCR_HOME;
static screen_t back_to = SCR_HOME;          /* where now-playing / Settings / a notice return to */
static screen_t browser_return = SCR_HOME;   /* where Back out of the browser goes: Home, or the
                                              * grid when an album was opened from it (0.16) */
static screen_t page_return = SCR_ALBUMS;    /* where Back out of the album page (or an album's list) goes: the grid, the
                                              * browser when an album folder was opened there (0.17b), or a folder's
                                              * list of albums (0.19a: page_return_folder) */
static char * page_return_folder;
static char * argv0;
static char restart_page[32];                /* set by a setting that needs a fresh start */

/* Folder views (0.19, Note 05 section 7): a folder opened from the browser
 * shows as its type's view - an album page, a grid of covers or artist
 * tiles, or a track list - and Back returns to where it was opened from.
 * A folder opened from a folder grid's tile pushes its parent here, so
 * Back climbs back through the views with the cursor on the folder left. */
#define FOLDER_STACK 16
static char * folder_stack[FOLDER_STACK];
static int    folder_depth;
static screen_t folder_root = SCR_BROWSER;  /* where the first folder view was opened from */
static screen_t list_return = SCR_HOME;     /* where Back out of a list goes */

/* Artists (0.20, Note 05 step 8): the view from Home is the rows (rows.c),
 * the grid of artist tiles or the list page, by style-artists-<kind>; an
 * artist opens as a grid of their albums or a list of their tracks
 * (opens-artists-<kind>), and Back from that returns to the artists view
 * it came from with the cursor on the artist. */
static screen_t artists_return = SCR_ROWS;  /* which artists view an artist was opened from */
static char *   artist_return_name;
static bool     page_to_artists;            /* the album page was opened straight from an artist (one album, 0.20c): Back returns there */
static bool     opening_from_artist;

static void enter_nowplaying(void);
static void show_home(void);
static void show_browser(void);
static bool browser_folder_hook(const char * uri);

static void leave_current(void)
{
    switch(cur) {
        case SCR_HOME:       home_hide(); break;
        case SCR_NOWPLAYING: nowplaying_hide(); break;
        case SCR_SETTINGS:   settings_hide(); break;
        case SCR_NOTICE:     notice_hide(); break;
        case SCR_ALBUMS:     grid_hide(); break;
        case SCR_ALBUMPAGE:  albumpage_hide(); break;
        case SCR_LIST:       listpage_hide(); break;
        case SCR_ROWS:       rows_hide(); break;
        default: break;
    }
}

static void show_rows(bool fresh)
{
    leave_current();
    cur = SCR_ROWS;
    rows_show(fresh);
}

static void show_list(bool fresh)
{
    leave_current();
    cur = SCR_LIST;
    listpage_show(fresh);
}

static void show_albums(bool fresh)
{
    leave_current();
    cur = SCR_ALBUMS;
    grid_show(fresh);
}

static void folder_stack_clear(void)
{
    while(folder_depth) free(folder_stack[--folder_depth]);
}

/* The Albums view from Home: the grid over every album. */
static void open_albums_view(void)
{
    folder_stack_clear();
    grid_set_scope(GRID_SCOPE_ALBUMS, NULL);
    show_albums(true);
}

/* The Tracks view from Home: the list page draws every style of it since
 * 0.19a (its Grid is two columns of text cells). */
static void open_tracks_view(void)
{
    folder_stack_clear();
    list_return = SCR_HOME;
    listpage_open(LIST_TRACKS, NULL);
    show_list(true);
}

static void open_recent_view(void)
{
    folder_stack_clear();
    list_return = SCR_HOME;
    listpage_open(LIST_RECENT, NULL);
    show_list(true);
}

/* Favorites, Mixtapes and Collections from Home (0.21): lists; a mixtape
 * opens as its list, a collection as a grid of its albums. */
static void open_curation_view(list_kind_t k)
{
    folder_stack_clear();
    list_return = SCR_HOME;
    listpage_open(k, NULL);
    show_list(true);
}

static void open_list(int uid)
{
    const cur_list_t * l = curation_by_uid(uid);
    if(!l) return;
    if(l->kind == CUR_MIXTAPE) { if(listpage_open_mixtape(uid)) show_list(true); }
    else { char key[300]; curation_key(uid, key, sizeof(key)); grid_set_scope(GRID_SCOPE_COLLECTION, key); show_albums(true); }
}

/* Back out of a mixtape or a collection: the list it was chosen from, on it. */
static void curation_view_exited(const char * key)
{
    char * k = key ? strdup(key) : NULL;
    listpage_open(cur == SCR_ALBUMS ? LIST_COLLECTIONS : LIST_MIXTAPES, NULL);
    show_list(false);
    if(k) listpage_focus_key(k);
    free(k);
}

/* A favourite, mixtape or collection changed (0.21): every screen that
 * shows them follows. */
static void curation_changed(void)
{
    albumpage_curation_changed();
    listpage_curation_changed();
    grid_curation_changed();
    nowplaying_curation_changed();
}

/* The Artists view from Home (0.20): rows, the grid or the list, as the
 * style setting says - the three faces share one setting. */
static const char * artists_style(void)
{
    static char v[16];
    char key[64];
    snprintf(key, sizeof(key), "style-artists-%s", ui_screen_kind_name());
    config_read(key, v, sizeof(v), "rows");
    return v;
}

static void show_artists(const char * style, bool fresh)
{
    if(strcmp(style, "grid") == 0) { grid_set_scope(GRID_SCOPE_ARTISTS, NULL); show_albums(fresh); }
    else if(strcmp(style, "rows") == 0) show_rows(fresh);
    else { list_return = SCR_HOME; listpage_open(LIST_ARTISTS, NULL); show_list(fresh); }
}

static void open_artists_view(void)
{
    folder_stack_clear();
    show_artists(artists_style(), true);
}

static void open_album(const library_t * lib, int album, const char * card, const tile_style_t * tile);

/* Select on an artist, in any of the three views: what "Opens as" says. */
static void open_artist(int artist)
{
    const artist_t * a = artists_get(artist);
    if(!a) return;
    free(artist_return_name);
    artist_return_name = strdup(a->name);
    artists_return = cur;
    char key[64], v[16];
    snprintf(key, sizeof(key), "opens-artists-%s", ui_screen_kind_name());
    config_read(key, v, sizeof(v), "albums");
    if(strcmp(v, "tracks") == 0 || strcmp(v, "tracks-album") == 0) {
        if(!listpage_open_artist_tracks(artist, strcmp(v, "tracks-album") == 0)) return;
        list_return = artists_return;
        show_list(true);
        return;
    }
    if(a->nalbums == 1) {
        /* one album: no grid of one between - straight to its page (Ian, 14 Oct) */
        const lib_source_t * src = libs_get(a->albums[0].src);
        opening_from_artist = true;
        open_album(&src->lib, a->albums[0].album, src->card, NULL);
        opening_from_artist = false;
        return;
    }
    grid_set_scope(GRID_SCOPE_ARTIST, a->name);
    show_albums(true);
}

/* Back out of an artist's albums or tracks: the artists view it was opened
 * from, with the cursor on the artist. */
static void artist_view_exited(void)
{
    const char * name = artist_return_name;
    screen_t to = artists_return;
    if(to == SCR_ALBUMS) { grid_set_scope(GRID_SCOPE_ARTISTS, NULL); show_albums(false); if(name) grid_focus_key(name); }
    else if(to == SCR_LIST) { listpage_open(LIST_ARTISTS, NULL); show_list(false); if(name) listpage_focus_key(name); }
    else { show_rows(false); if(name) rows_focus_artist(artists_find(name)); }
}

static void rows_exited(void) { show_home(); }

/* A folder's children as the grid or as rows, whichever the folder view's
 * style says (0.19a). */
static bool show_folder_children(const char * uri, bool fresh)
{
    char key[64], v[16];
    snprintf(key, sizeof(key), "style-folder-%s", ui_screen_kind_name());
    config_read(key, v, sizeof(v), "grid");
    if(strcmp(v, "grid") != 0 && listpage_open(LIST_ALBUMS, uri)) { show_list(fresh); return true; }
    grid_set_scope(GRID_SCOPE_FOLDER, uri);
    show_albums(fresh);
    return true;
}

/* The folder view's picker chose the other face: the same folder as rows
 * or as the grid, the panel staying up on its View row so the row can be
 * stepped on (Ian, 13 Oct: it went away under the scroll wheel). */
static void view_style_changed(const char * style)
{
    bool to_grid = strcmp(style, "grid") == 0;
    /* the Artists faces hand over among themselves (0.20) */
    bool from_artists = cur == SCR_ROWS || (cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_ARTISTS) || (cur == SCR_LIST && listpage_kind() == LIST_ARTISTS);
    if(from_artists) {
        show_artists(style, true);
        if(cur == SCR_ROWS) rows_show_picker_style();
        else if(cur == SCR_ALBUMS) grid_show_picker_style();
        else listpage_show_picker_style();
        return;
    }
    if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_FOLDER && !to_grid) {
        if(!listpage_open(LIST_ALBUMS, grid_scope_folder())) return;
        show_list(true);
        listpage_show_picker_style();
    }
    else if(cur == SCR_LIST && listpage_kind() == LIST_ALBUMS && to_grid) {
        grid_set_scope(GRID_SCOPE_FOLDER, listpage_folder());
        show_albums(true);
        grid_show_picker_style();
    }
}

/* The folder the view on screen shows, for the Back chain. */
static const char * current_folder_view(void)
{
    if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_FOLDER) return grid_scope_folder();
    if(cur == SCR_LIST && (listpage_kind() == LIST_ALBUMS || listpage_kind() == LIST_FOLDER)) return listpage_folder();
    return NULL;
}

static void push_current_folder(void)
{
    const char * f = current_folder_view();
    if(f && folder_depth < FOLDER_STACK) folder_stack[folder_depth++] = strdup(f);
}

/* Open a folder as its type's view (0.19): an album folder as the album
 * page (the hook), a collection folder as a mixtape-style track list, an
 * artist or library folder as a grid of its children. False when the
 * folder is none of these (the browser lists it). `from_grid`: opened
 * from a folder grid's tile, so Back climbs back to that grid. */
static bool open_folder_view(const char * uri, bool from_grid)
{
    libs_refresh();
    const lib_folder_t * f;
    if(!libs_find_folder(uri, &f)) return false;
    switch(f->type) {
        case FOLDER_ALBUM:
            return browser_folder_hook(uri);
        case FOLDER_COLLECTION:
            if(!listpage_open(LIST_FOLDER, uri)) return false;
            if(from_grid) push_current_folder();
            else { folder_stack_clear(); folder_root = SCR_BROWSER; }
            list_return = from_grid ? SCR_ALBUMS : SCR_BROWSER;
            show_list(true);
            return true;
        case FOLDER_ARTIST:
        case FOLDER_LIBRARY:
            if(from_grid) push_current_folder();
            else { folder_stack_clear(); folder_root = SCR_BROWSER; }
            return show_folder_children(uri, true);
        default:
            return false;
    }
}

/* Select on a folder tile in a folder grid, or a folder row in a folder's list. */
static void grid_open_folder_tile(const char * uri)
{
    screen_t from = cur;
    if(!open_folder_view(uri, true) && browser_open_folder(uri)) { browser_return = from == SCR_LIST ? SCR_LIST : SCR_ALBUMS; show_browser(); }
}

/* Back from a folder view to the browser: at the folder's parent, on the
 * folder's row (the browser may have been moved since - a plain folder
 * opened from a grid tile - so its stack is rebuilt from the path). */
static void back_to_browser(const char * folder)
{
    if(folder) {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", folder);
        char * slash = strrchr(parent, '/');
        if(slash) *slash = 0; else parent[0] = 0;
        if(parent[0] && strcmp(browser_current_uri(), parent) != 0) browser_open_path(parent, folder);
        else if(!parent[0] && browser_current_uri()[0]) { browser_open_root(); }
    }
    show_browser();
}

/* Back out of a folder's view (its grid, its rows, or a collection's
 * list): the parent folder's view with the cursor on the folder just
 * left, the browser it was opened from, or Home. */
static void folder_view_exited(const char * folder_now)
{
    if(folder_depth > 0) {
        char * child = strdup(folder_now);
        char * parent = folder_stack[--folder_depth];
        show_folder_children(parent, false);
        free(parent);
        if(child) {
            if(cur == SCR_ALBUMS) grid_focus_key(child); else listpage_focus_key(child);
            free(child);
        }
        return;
    }
    if(folder_root == SCR_BROWSER || list_return == SCR_BROWSER) { back_to_browser(folder_now); return; }
    show_home();
}

/* Back out of a grid (Albums or a folder's children). */
static void grid_exited(void)
{
    if(grid_scope() == GRID_SCOPE_FOLDER) { folder_view_exited(grid_scope_folder()); return; }
    if(grid_scope() == GRID_SCOPE_ARTIST) { artist_view_exited(); return; }
    if(grid_scope() == GRID_SCOPE_COLLECTION) { curation_view_exited(grid_scope_collection()); return; }
    show_home();
}

static void albumpage_exited(void);

/* Back out of a list page. */
static void list_exited(void)
{
    switch(listpage_kind()) {
        case LIST_FOLDER: case LIST_ALBUMS: folder_view_exited(listpage_folder()); return;
        case LIST_ALBUM: albumpage_exited(); return;        /* an album's list leaves like its page (from an artist too) */
        case LIST_ARTIST_TRACKS: artist_view_exited(); return;
        case LIST_MIXTAPE: curation_view_exited(listpage_folder()); return;
        default: show_home(); return;
    }
}

/* A track started from a list or the Tracks grid: the handheld shows
 * now-playing (the TV stays on the list, the mark follows). */
static void list_played(void)
{
    if(ui_screen.kind != UI_SCREEN_TV) enter_nowplaying();
}

static void show_home(void)
{
    leave_current();
    cur = SCR_HOME;
    home_show();
}

static void show_albumpage(bool fresh)
{
    leave_current();
    cur = SCR_ALBUMPAGE;
    albumpage_show(fresh);
}

static void show_browser(void)
{
    leave_current();
    cur = SCR_BROWSER;
    lv_screen_load(browser_scr);
    browser_shown();
}

/* Back out of the browser: Home, or the grid it was opened from. */
static void browser_exited(void)
{
    if(browser_return == SCR_ALBUMS) { browser_return = SCR_HOME; show_albums(false); }
    else if(browser_return == SCR_LIST) { browser_return = SCR_HOME; show_list(false); }
    else if(browser_return == SCR_ROWS) { browser_return = SCR_HOME; show_rows(false); }
    else show_home();
}

/* The grid (or a folder's list of albums) chose an album: the album page
 * (0.17), or, for an album of many artists - a soundtrack, a compilation -
 * the mixtape-style list (0.19a, Ian: "albums with various artists should
 * look the same way"); Back returns to where it was chosen. An album the
 * page can't take (no tracks) opens as its folder in the browser instead. */
static void set_page_return(void)
{
    free(page_return_folder);
    page_return_folder = NULL;
    page_to_artists = opening_from_artist;
    if(cur == SCR_LIST && listpage_kind() == LIST_ALBUMS) { page_return = SCR_LIST; page_return_folder = strdup(listpage_folder()); }
    else if(cur == SCR_BROWSER) page_return = SCR_BROWSER;
    else if(cur == SCR_ROWS) page_return = SCR_ROWS;
    else page_return = SCR_ALBUMS;
}

static void open_album(const library_t * lib, int album, const char * card, const tile_style_t * tile)
{
    set_page_return();
    if(lib && album >= 0 && album < lib->nalbums && lib->albums[album].various && listpage_open_album(card, album)) { show_list(true); return; }
    if(albumpage_open(lib, album, card, tile)) { show_albumpage(true); return; }
    const char * folder = (lib && album >= 0 && album < lib->nalbums) ? lib->albums[album].folder : NULL;
    if(!folder || !browser_open_folder(folder)) return;        /* unreadable just now: stay where we are */
    browser_return = cur == SCR_LIST ? SCR_LIST : cur == SCR_ROWS ? SCR_ROWS : SCR_ALBUMS;
    show_browser();
}

/* Back out of the album page (or an album's list): the grid, on the same
 * album; the folder's list of albums it was chosen from; or the browser
 * when the album was opened as a folder there. */
static void albumpage_exited(void)
{
    if(page_to_artists) { page_to_artists = false; artist_view_exited(); return; }
    const char * folder = cur == SCR_LIST ? listpage_folder() : albumpage_folder();
    char * child = folder ? strdup(folder) : NULL;
    if(page_return == SCR_BROWSER) show_browser();
    else if(page_return == SCR_ROWS) show_rows(false);        /* the rows keep their place */
    else if(page_return == SCR_LIST && page_return_folder && listpage_open(LIST_ALBUMS, page_return_folder)) {
        show_list(false);
        if(child) listpage_focus_key(child);
    }
    else { show_albums(false); if(child) grid_focus_key(child); }
    free(child);
}

/* The handheld page started a track: now-playing, as after the browser. */
static void albumpage_played(void)
{
    enter_nowplaying();
}

/* The browser is about to enter a folder: one that holds a single album
 * opens as the album page instead (Note 05 section 7). */
static bool browser_folder_hook(const char * uri)
{
    const library_t * lib;
    const char * card;
    const tile_style_t * tile;
    int album;
    if(!grid_lookup_folder(uri, &lib, &album, &card, &tile)) return false;
    if(lib->albums[album].various) {
        /* many artists: the mixtape-style list, like a collection folder (0.19a) */
        if(!listpage_open_album(card, album)) return false;
        set_page_return();
        show_list(true);
        return true;
    }
    if(!albumpage_open_folder(lib, uri, album, card, tile)) return false;   /* the folder's own tracks, in file order (Ian, 11 Oct) */
    set_page_return();
    show_albumpage(true);
    return true;
}

/* The browser is about to enter a folder (0.19): its type's view, else
 * the browser's own list. */
static bool browser_enter_hook(const char * uri)
{
    return open_folder_view(uri, false);
}

static void enter_nowplaying(void)
{
    if(cur == SCR_NOWPLAYING) return;
    back_to = (cur == SCR_NOTICE) ? SCR_HOME : cur;
    leave_current();
    cur = SCR_NOWPLAYING;
    nowplaying_show();
}

static void enter_settings(const char * page, bool from_view)
{
    if(cur == SCR_SETTINGS) return;
    back_to = (cur == SCR_NOTICE || cur == SCR_NOWPLAYING) ? SCR_HOME : cur;
    if(cur == SCR_NOWPLAYING) back_to = SCR_NOWPLAYING;
    leave_current();
    cur = SCR_SETTINGS;
    settings_show(page, from_view);
}

static void enter_notice(const char * title, const char * text)
{
    back_to = cur == SCR_NOTICE ? SCR_HOME : cur;
    leave_current();
    cur = SCR_NOTICE;
    notice_show(title, text);
}

/* Return from a screen that was entered from another. */
static void go_back(void)
{
    screen_t to = back_to;
    back_to = SCR_HOME;
    switch(to) {
        case SCR_BROWSER:    show_browser(); break;
        case SCR_NOWPLAYING: enter_nowplaying(); break;
        case SCR_ALBUMS:     show_albums(false); break;
        case SCR_ALBUMPAGE:  show_albumpage(false); break;
        case SCR_LIST:       show_list(false); break;
        case SCR_ROWS:       show_rows(false); break;
        default:             show_home(); break;
    }
}

/* The Home page chose a view. */
static void open_view(home_view_t v)
{
    switch(v) {
        case HOME_FOLDERS:
            /* the browser may still be sitting in an album opened from the
             * grid: Folders is the library from the top */
            browser_return = SCR_HOME;
            if(browser_opened_at_folder()) browser_open_root();
            show_browser();
            break;
        case HOME_ALBUMS:      open_albums_view(); break;
        case HOME_ARTISTS:     open_artists_view(); break;
        case HOME_TRACKS:      open_tracks_view(); break;
        case HOME_RECENT:      open_recent_view(); break;
        case HOME_FAVORITES:   open_curation_view(LIST_FAVORITES); break;
        case HOME_PLAYLISTS:   open_curation_view(LIST_MIXTAPES); break;
        case HOME_COLLECTIONS: open_curation_view(LIST_COLLECTIONS); break;
        case HOME_NOW_PLAYING: enter_nowplaying(); break;
        case HOME_SETTINGS:    enter_settings("hub", false); break;
        default: {
            char title[160];
            snprintf(title, sizeof(title), "%s: %s", home_view_name(v), T(S_VIEW_NOT_YET));
            enter_notice(title, T(S_VIEW_NOT_YET_TEXT));
            break;
        }
    }
}

/* A long press on a Home tape (0.20c): the view, with its picker panel up
 * on the View row, to choose the view and the sort before browsing. */
static void open_view_with_picker(home_view_t v)
{
    open_view(v);
    switch(cur) {
        case SCR_ALBUMS:  grid_show_picker_style(); break;
        case SCR_LIST:    listpage_show_picker_style(); break;
        case SCR_ROWS:    rows_show_picker_style(); break;
        case SCR_BROWSER: if(!browser_overlay_active()) browser_toggle_picker(); picker_focus_style(); break;
        default: break;
    }
}

/* The browser started a track: show it. */
static void on_play(void)
{
    enter_nowplaying();
}

/* ---- action dispatch ---- */

/* A play pressed while MPD is away (restarting for a new DAC, 12 Oct: four
 * seconds in which a press used to vanish) is kept and played when it is
 * back. */
static bool pending_play;

static void transport(ui_action_t a)
{
    if(!mpd_is_connected()) {
        if(a == ACT_PLAYPAUSE || a == ACT_PLAY) { pending_play = true; nowplaying_mpd_away(); }
        return;
    }
    switch(a) {
        case ACT_PLAYPAUSE: mpd_toggle_pause(); break;
        /* a long track (a podcast, an audiobook, a 25-minute side): X moves
         * 30 s instead of a whole track (Ian, 12 Oct) */
        case ACT_NEXT:      if(power_skip_is_seek()) mpd_seek_relative(POWER_SEEK_STEP_S);  else mpd_next();     break;
        case ACT_PREV:      if(power_skip_is_seek()) mpd_seek_relative(-POWER_SEEK_STEP_S); else mpd_previous(); break;
        default: break;
    }
}

static void dispatch(ui_action_t a)
{
    /* the power menu takes everything while it is up */
    if(power_menu_active()) { power_activity(a); power_menu_handle_action(a); return; }
    /* so do the keyboard and the object menu (0.21) */
    if(keyboard_active()) { if(power_activity(a)) return; keyboard_handle_action(a); return; }
    if(objmenu_active()) { if(power_activity(a)) return; objmenu_handle_action(a); return; }
    /* a button in the dark: X and Y act (pause, skip), anything else only
     * lights the screen and is spent on that (0.18) */
    if(power_activity(a)) return;

    /* transport keys work on every screen */
    switch(a) {
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV: transport(a); break;
        /* Remote-only explicit play/pause (cec.c), consumed here so no screen
         * sees an action it does not know; now-playing catches up on its poll. */
        case ACT_PLAY:      mpd_resume();       return;
        case ACT_PAUSE:     mpd_pause();        return;
        case ACT_HOME:
            if(cur == SCR_NOWPLAYING) go_back(); else enter_nowplaying();
            return;
        case ACT_HOMEPAGE:                      /* the F button (0.21a): Home from anywhere */
            if(cur != SCR_HOME) { folder_stack_clear(); back_to = SCR_HOME; show_home(); }
            return;
        case ACT_SETTINGS:
            switch(cur) {
                /* a toggle, like the now-playing button: press again, it goes */
                case SCR_BROWSER:    browser_toggle_picker(); break;       /* the view picker (0.15) */
                case SCR_ALBUMS:     grid_toggle_picker(); break;
                case SCR_LIST:       listpage_toggle_picker(); break;
                case SCR_ROWS:       rows_toggle_picker(); break;
                case SCR_NOWPLAYING: enter_settings("playback", true); break;
                case SCR_HOME:       enter_settings("hub", false); break;
                case SCR_SETTINGS:   go_back(); break;
                default: break;
            }
            return;
        default: break;
    }

    switch(cur) {
        case SCR_HOME:
            home_handle_action(a);
            return;
        case SCR_NOWPLAYING:
            if(nowplaying_handle_action(a) == NP_EXIT) go_back();
            return;
        case SCR_SETTINGS:
            if(settings_handle_action(a) == SET_EXIT) go_back();
            return;
        case SCR_NOTICE:
            if(notice_handle_action(a)) go_back();
            return;
        case SCR_ALBUMS:
            grid_handle_action(a);
            return;
        case SCR_ALBUMPAGE:
            albumpage_handle_action(a);
            return;
        case SCR_LIST:
            listpage_handle_action(a);
            return;
        case SCR_ROWS:
            rows_handle_action(a);
            return;
        case SCR_BROWSER:
            break;
    }

    /* the picker or the strip takes everything while it is up (0.14) */
    if(browser_overlay_active()) { browser_handle_action(a); return; }

    switch(a) {
        case ACT_UP:        key_tap(LV_KEY_PREV);  break;
        case ACT_DOWN:      key_tap(LV_KEY_NEXT);  break;
        case ACT_SELECT:    key_tap(LV_KEY_ENTER); break;
        case ACT_BACK:      browser_handle_action(a); break;     /* up a folder, the strip, or Home */
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV: break;   /* handled above */
        case ACT_VOL_UP:
        case ACT_VOL_DOWN:  /* Fixed-volume mode: overlay comes with Settings work */ break;
        default:            browser_handle_action(a); break;     /* left/right paging, menu */
    }
}

/* Same program, same screen and folder: a new output, or a setting that is
 * read at start-up. The DRM and uevent fds are close-on-exec; anything else
 * we opened (the MPD socket) is closed here so a run of hotplugs can't pile
 * up descriptors. */
static void restart_ui(void)
{
    const char * folder = browser_current_uri();
    char * args[12];
    int n = 0;
    args[n++] = argv0;
    if(folder && folder[0]) { args[n++] = "--folder"; args[n++] = (char *)folder; }
    if(cur == SCR_NOWPLAYING) args[n++] = "--nowplaying";
    else if(cur == SCR_BROWSER) args[n++] = "--browser";
    else if(cur == SCR_ROWS) args[n++] = "--artists";
    else if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_FOLDER) { args[n++] = "--foldergrid"; args[n++] = (char *)grid_scope_folder(); }
    else if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_TRACKS) args[n++] = "--tracksgrid";
    else if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_ARTISTS) args[n++] = "--artistsgrid";
    else if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_ARTIST) { args[n++] = "--artist"; args[n++] = (char *)grid_scope_artist(); }
    else if(cur == SCR_ALBUMS && grid_scope() == GRID_SCOPE_COLLECTION) { args[n++] = "--collection"; args[n++] = (char *)grid_scope_collection(); }
    else if(cur == SCR_ALBUMS) args[n++] = "--albums";
    else if(cur == SCR_LIST && listpage_kind() == LIST_FOLDER) { args[n++] = "--folderlist"; args[n++] = (char *)listpage_folder(); }
    else if(cur == SCR_LIST && listpage_kind() == LIST_ALBUMS) { args[n++] = "--folderalbums"; args[n++] = (char *)listpage_folder(); }
    else if(cur == SCR_LIST && listpage_kind() == LIST_ALBUM) { args[n++] = "--albumlist"; args[n++] = (char *)listpage_folder(); }
    else if(cur == SCR_LIST && listpage_kind() == LIST_ARTISTS) args[n++] = "--artistslist";
    else if(cur == SCR_LIST && listpage_kind() == LIST_ARTIST_TRACKS && artists_get(listpage_artist())) { args[n++] = "--artisttracks"; args[n++] = artists_get(listpage_artist())->name; }
    else if(cur == SCR_LIST && listpage_kind() == LIST_MIXTAPE) { args[n++] = "--mixtape"; args[n++] = (char *)listpage_folder(); }
    else if(cur == SCR_LIST && listpage_kind() == LIST_FAVORITES) args[n++] = "--favorites";
    else if(cur == SCR_LIST && listpage_kind() == LIST_MIXTAPES) args[n++] = "--mixtapes";
    else if(cur == SCR_LIST && listpage_kind() == LIST_COLLECTIONS) args[n++] = "--collections";
    else if(cur == SCR_LIST) args[n++] = listpage_kind() == LIST_RECENT ? "--recent" : "--tracks";
    else if(cur == SCR_ALBUMPAGE && albumpage_folder()) { args[n++] = albumpage_by_folder() ? "--folderpage" : "--album"; args[n++] = (char *)albumpage_folder(); }
    if(restart_page[0]) { args[n++] = "--settings"; args[n++] = restart_page; }
    args[n] = NULL;
    for(int fd = 3; fd < 256; fd++) close(fd);
    execv("/proc/self/exe", args);
    _exit(1);                                   /* systemd brings us back */
}

/* A setting that needs a fresh start (settings.c): restart after this
 * action is fully handled, back on the same settings page. */
static void restart_for_setting(const char * page)
{
    snprintf(restart_page, sizeof(restart_page), "%s", page ? page : "hub");
}

/* A view's options changed on its settings page: the view takes them now. */
static void view_options_changed(const char * view)
{
    if(strcmp(view, "folders") == 0) browser_reload_options();
    if(strcmp(view, "albums") == 0) grid_reload_options();
    if(strcmp(view, "tracks") == 0 || strcmp(view, "recent") == 0 || strcmp(view, "mixtape") == 0) listpage_reload_options();
    if(strcmp(view, "artists") == 0) { rows_reload_options(); grid_reload_options(); listpage_reload_options(); }
}

/* Hand MPD's change notices to whoever shows or acts on that state. */
static void mpd_changed(unsigned what)
{
    if(what & (MPD_CHANGED_DATABASE | MPD_CHANGED_UPDATE))
        browser_library_changed(what & MPD_CHANGED_DATABASE);
    if(what & MPD_CHANGED_DATABASE) { grid_library_changed(); listpage_library_changed(); rows_library_changed(); }
    if(what & MPD_CHANGED_PLAYER) { albumpage_mpd_changed(); listpage_mpd_changed(); history_mpd_changed(); dacpower_mpd_changed(); }
    if(what & (MPD_CHANGED_PLAYER | MPD_CHANGED_MIXER | MPD_CHANGED_OPTIONS |
               MPD_CHANGED_PLAYLIST | MPD_CHANGED_OUTPUT))
        nowplaying_mpd_changed();
    if(what & (MPD_CHANGED_PLAYER | MPD_CHANGED_PLAYLIST)) home_mpd_changed();
    if(what & (MPD_CHANGED_DATABASE | MPD_CHANGED_UPDATE)) settings_mpd_changed();
    playthrough_mpd_changed(what);
}

static void mpd_retry_cb(lv_timer_t * t)
{
    (void)t;
    browser_tick();
    /* Not connected for notices (start-up before MPD, or MPD restarted):
     * try again. Anything may have changed while we weren't listening. */
    if(mpd_idle_start()) {
        mpd_changed(MPD_CHANGED_ALL);
        if(pending_play && mpd_is_connected()) { pending_play = false; mpd_resume(); }
    }
}

/* Back from a sleep: whatever changed while the unit was frozen (a DAC
 * gone, a card swapped) comes in through the usual notices; ask for all. */
static void after_resume(void)
{
    mpd_changed(MPD_CHANGED_ALL);
}

/* Just before a clean power-off: nothing of ours needs saving today (MPD
 * keeps its own state); the hook is here for when something does. */
static void before_poweroff(void)
{
}

/* A crash prints where it happened (0.16c, 8 Oct: the UI died twice on the
 * unit with MPD playing on, and nothing said where). The frames go to
 * stderr and to /storage/simpleton-crash.txt, then the signal is re-raised
 * so the exit status still says "crashed". Only async-signal-safe calls
 * here; the function names come from -rdynamic in package.mk. */
static void crash_handler(int sig)
{
    void * frames[48];
    int n = backtrace(frames, 48);
    static const char head[] = "simpleton-ui " SIMPLETON_VERSION ": crashed, signal ";
    char num[4] = { (char)('0' + sig / 10), (char)('0' + sig % 10), '\n', 0 };
    int fds[2] = { 2, open("/storage/simpleton-crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644) };
    for(int i = 0; i < 2; i++) {
        if(fds[i] < 0) continue;
        (void)!write(fds[i], head, sizeof(head) - 1);
        (void)!write(fds[i], num, 3);
        backtrace_symbols_fd(frames, n, fds[i]);
    }
    if(fds[1] >= 0) close(fds[1]);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_handler_install(void)
{
    /* an alternate stack, so a blown stack still reports */
    static char altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL };
    for(size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}

int main(int argc, char ** argv)
{
    crash_handler_install();
    const char * resume_folder = NULL, * resume_settings = NULL, * resume_album = NULL, * resume_folderpage = NULL;
    const char * resume_foldergrid = NULL, * resume_folderlist = NULL, * resume_folderalbums = NULL, * resume_albumlist = NULL;
    const char * resume_artist = NULL, * resume_artisttracks = NULL, * resume_mixtape = NULL, * resume_collection = NULL;
    bool resume_favorites = false, resume_mixtapes = false, resume_collections = false;
    bool resume_nowplaying = false, resume_browser = false, resume_albums = false, resume_tracks = false, resume_tracksgrid = false, resume_recent = false;
    bool resume_artists = false, resume_artistsgrid = false, resume_artistslist = false;
    argv0 = argv[0];
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--folder") == 0 && i + 1 < argc) resume_folder = argv[++i];
        else if(strcmp(argv[i], "--nowplaying") == 0) resume_nowplaying = true;
        else if(strcmp(argv[i], "--browser") == 0) resume_browser = true;
        else if(strcmp(argv[i], "--albums") == 0) resume_albums = true;
        else if(strcmp(argv[i], "--album") == 0 && i + 1 < argc) resume_album = argv[++i];
        else if(strcmp(argv[i], "--folderpage") == 0 && i + 1 < argc) resume_folderpage = argv[++i];
        else if(strcmp(argv[i], "--foldergrid") == 0 && i + 1 < argc) resume_foldergrid = argv[++i];
        else if(strcmp(argv[i], "--folderlist") == 0 && i + 1 < argc) resume_folderlist = argv[++i];
        else if(strcmp(argv[i], "--folderalbums") == 0 && i + 1 < argc) resume_folderalbums = argv[++i];
        else if(strcmp(argv[i], "--albumlist") == 0 && i + 1 < argc) resume_albumlist = argv[++i];
        else if(strcmp(argv[i], "--tracks") == 0) resume_tracks = true;
        else if(strcmp(argv[i], "--tracksgrid") == 0) resume_tracksgrid = true;
        else if(strcmp(argv[i], "--recent") == 0) resume_recent = true;
        else if(strcmp(argv[i], "--artists") == 0) resume_artists = true;
        else if(strcmp(argv[i], "--artistsgrid") == 0) resume_artistsgrid = true;
        else if(strcmp(argv[i], "--artistslist") == 0) resume_artistslist = true;
        else if(strcmp(argv[i], "--artist") == 0 && i + 1 < argc) resume_artist = argv[++i];
        else if(strcmp(argv[i], "--artisttracks") == 0 && i + 1 < argc) resume_artisttracks = argv[++i];
        else if(strcmp(argv[i], "--favorites") == 0) resume_favorites = true;
        else if(strcmp(argv[i], "--mixtapes") == 0) resume_mixtapes = true;
        else if(strcmp(argv[i], "--collections") == 0) resume_collections = true;
        else if(strcmp(argv[i], "--mixtape") == 0 && i + 1 < argc) resume_mixtape = argv[++i];
        else if(strcmp(argv[i], "--collection") == 0 && i + 1 < argc) resume_collection = argv[++i];
        else if(strcmp(argv[i], "--settings") == 0 && i + 1 < argc) resume_settings = argv[++i];
        /* the built-in English table as a language file: the master copy of
         * share/lang/en.txt and the template for a new language */
        else if(strcmp(argv[i], "--dump-lang") == 0) { strings_dump(stdout); return 0; }
    }

    strings_init();  /* interface text; built-in English if the file is missing */

    lv_init();

    display_info_t di;
    lv_display_t * disp = display_init(&di);
    if(disp == NULL) { fprintf(stderr, "simpleton-ui: display init failed\n"); return 1; }
    layout_init(di.w, di.h, di.external);
    if(di.external) hdmi_silence_start();     /* zeros on the TV's audio link: no hum (0.17j) */
    {
        ui_grid_t g;
        ui_list_t l;
        ui_grid_metrics(UI_SIZE_DEFAULT, &g);
        ui_list_metrics(UI_SIZE_DEFAULT, &l);
        fprintf(stderr, "simpleton-ui: layout: %s %dx%d, stage %d; size %s = grid %dx%d of %d px covers, list rows %d px\n",
                ui_screen_kind_name(), ui_screen.w, ui_screen.h, ui_screen.stage,
                ui_size_name(UI_SIZE_DEFAULT), g.rows, g.cols, g.tile, l.row_h);
    }
    fonts_init();    /* FreeType + Noto fallbacks; falls back to built-in fonts if it can't */

    /* Dark default theme with our accent and list font, so anything we
     * forget to style still lands on the right side of readable. */
    lv_theme_t * th = lv_theme_default_init(disp, lv_color_hex(UI_COLOR_FOCUS_BG),
                                            lv_color_hex(UI_COLOR_DIM), true, UI_FONT_LIST);
    lv_display_set_theme(disp, th);

    /* Keypad input device bound to one focus group. */
    lv_group_t * grp = lv_group_create();
    lv_group_set_default(grp);
    lv_indev_t * kb = lv_indev_create();
    lv_indev_set_type(kb, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb, keypad_read_cb);
    lv_indev_set_group(kb, grp);

    if(!input_init()) fprintf(stderr, "simpleton-ui: running without joypad input\n");
    cec_init();      /* optional: TV remote via HDMI-CEC; quiet no-op without it */
    power_set_external(di.external);             /* first: on a TV the panel is left alone */
    power_init(after_resume, before_poweroff);   /* battery, screen, sleep, the power key (0.18) */

    mpd_connect();   /* may fail: browser shows "Starting library" and retries */
    mpd_idle_start();
    dacpower_init();                         /* wake the DAC on play, let it sleep when idle (0.21c) */
    dacpower_mpd_changed();/* change notices; retried with the connection if MPD isn't up */
    browser_scr = lv_screen_active();
    browser_create(browser_scr, grp, on_play, browser_exited, resume_folder);
    grid_create(open_album, grid_exited);
    grid_set_callbacks(grid_open_folder_tile, list_played, view_style_changed);
    listpage_create(list_exited, list_played, view_style_changed);
    listpage_set_callbacks(open_album, grid_open_folder_tile);
    grid_set_artist_callback(open_artist);
    listpage_set_artist_callback(open_artist);
    listpage_set_list_callback(open_list);
    objmenu_create(curation_changed);
    keyboard_create();                               /* naming mixtapes and collections (0.21a) */                /* A held: Favorite / Add to mixtape / Add to collection (0.21) */
    nowplaying_set_curation_callback(curation_changed);
    rows_create(open_artist, open_album, rows_exited, view_style_changed);   /* the Artists rows (0.20) */
    history_init(listpage_history_changed);          /* what was played, for Recent (0.19a) */
    albumpage_create(albumpage_exited, albumpage_played);
    browser_set_folder_hook(browser_enter_hook);
    nowplaying_create();
    settings_create(restart_for_setting, view_options_changed);
    notice_create();
    home_create(open_view);
    home_set_menu_callback(open_view_with_picker);
    lv_timer_create(mpd_retry_cb, 2000, NULL);
    /* the screen we were on before a restart; Home otherwise */
    cur = SCR_HOME;
    home_show();
    if(resume_browser) show_browser();
    if(resume_albums) { grid_set_scope(GRID_SCOPE_ALBUMS, NULL); show_albums(false); }
    if(resume_tracksgrid) { grid_set_scope(GRID_SCOPE_TRACKS, NULL); show_albums(false); }
    if(resume_foldergrid) { show_browser(); folder_root = SCR_BROWSER; grid_set_scope(GRID_SCOPE_FOLDER, resume_foldergrid); show_albums(false); }
    if(resume_tracks) { listpage_open(LIST_TRACKS, NULL); show_list(false); }
    if(resume_recent) { listpage_open(LIST_RECENT, NULL); show_list(false); }
    if(resume_artists) show_rows(false);
    if(resume_artistsgrid) { grid_set_scope(GRID_SCOPE_ARTISTS, NULL); show_albums(false); }
    if(resume_artistslist) { listpage_open(LIST_ARTISTS, NULL); show_list(false); }
    if(resume_favorites) { listpage_open(LIST_FAVORITES, NULL); show_list(false); }
    if(resume_mixtapes) { listpage_open(LIST_MIXTAPES, NULL); show_list(false); }
    if(resume_collections) { listpage_open(LIST_COLLECTIONS, NULL); show_list(false); }
    if(resume_mixtape) { libs_refresh(); int uid = curation_find_key(resume_mixtape); if(uid && listpage_open_mixtape(uid)) show_list(false); else fprintf(stderr, "simpleton-ui: --mixtape %s: not here now\n", resume_mixtape); }
    if(resume_collection) { libs_refresh(); if(curation_find_key(resume_collection)) { grid_set_scope(GRID_SCOPE_COLLECTION, resume_collection); show_albums(false); } else fprintf(stderr, "simpleton-ui: --collection %s: not here now\n", resume_collection); }
    if(resume_artist || resume_artisttracks) {
        /* an artist's albums or tracks: Back goes to the artists view the style setting names */
        const char * st = artists_style();
        artists_return = strcmp(st, "grid") == 0 ? SCR_ALBUMS : strcmp(st, "rows") == 0 ? SCR_ROWS : SCR_LIST;
        artists_refresh();
        int i = artists_find(resume_artist ? resume_artist : resume_artisttracks);
        if(i >= 0) {
            free(artist_return_name);
            artist_return_name = strdup(artists_get(i)->name);
            if(resume_artist) { grid_set_scope(GRID_SCOPE_ARTIST, artists_get(i)->name); show_albums(false); }
            else { char key[64], v[16]; snprintf(key, sizeof(key), "opens-artists-%s", ui_screen_kind_name()); config_read(key, v, sizeof(v), "albums");
                   if(listpage_open_artist_tracks(i, strcmp(v, "tracks-album") == 0)) { list_return = artists_return; show_list(false); } }
        }
        else fprintf(stderr, "simpleton-ui: --artist %s: not listed now\n", resume_artist ? resume_artist : resume_artisttracks);
    }
    if(resume_folderlist) { show_browser(); list_return = SCR_BROWSER; if(listpage_open(LIST_FOLDER, resume_folderlist)) show_list(false); }
    if(resume_folderalbums) { show_browser(); folder_root = SCR_BROWSER; if(listpage_open(LIST_ALBUMS, resume_folderalbums)) show_list(false); }
    if(resume_albumlist) {
        show_browser();
        const library_t * lib; const char * card; const tile_style_t * tile; int album;
        if(grid_lookup_folder(resume_albumlist, &lib, &album, &card, &tile) && listpage_open_album(card, album)) { page_return = SCR_BROWSER; show_list(false); }
        else fprintf(stderr, "simpleton-ui: --albumlist %s: not an album folder now\n", resume_albumlist);
    }
    if(resume_album) { grid_set_scope(GRID_SCOPE_ALBUMS, NULL); show_albums(false); if(!grid_open_folder(resume_album)) fprintf(stderr, "simpleton-ui: --album %s: not in the grid\n", resume_album); }
    if(resume_folderpage) { show_browser(); if(!browser_folder_hook(resume_folderpage)) fprintf(stderr, "simpleton-ui: --folderpage %s: not an album folder now\n", resume_folderpage); }
    if(resume_nowplaying) enter_nowplaying();
    if(resume_settings) enter_settings(resume_settings, false);
    fprintf(stderr, "simpleton-ui: play-through folders: %s\n", playthrough_enabled() ? "on" : "off");
    playthrough_mpd_changed(MPD_CHANGED_PLAYER);   /* restarted mid-album: catch up now */

    for(;;) {
        uint32_t idle_ms = lv_timer_handler();
        if(idle_ms > 20) idle_ms = 20;          /* keep hold/repeat timing tight */

        struct pollfd pfd[5] = { { .fd = input_fd(), .events = POLLIN },
                                 { .fd = display_hotplug_fd(), .events = POLLIN },
                                 { .fd = cec_fd(), .events = POLLIN | POLLPRI },
                                 { .fd = mpd_idle_fd(), .events = POLLIN },
                                 { .fd = power_key_fd(), .events = POLLIN } };
        poll(pfd, 5, (int)idle_ms);             /* negative fds are ignored by poll() */

        input_poll();
        cec_poll();                             /* TV remote keys join the same queue */
        power_poll();                           /* the power key: press, hold, countdown */
        dacpower_poll();                        /* the DAC's sleep, the validity probe (0.21c) */
        if(pfd[3].fd >= 0 && (pfd[3].revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned what = mpd_idle_poll();
            if(what & MPD_IDLE_LOST) fprintf(stderr, "simpleton-ui: mpd[idle]: connection lost, reconnecting\n");
            mpd_changed(what & MPD_CHANGED_ALL);
        }
        for(ui_action_t a; (a = input_next_action()) != ACT_NONE;) dispatch(a);

        if(display_hotplug_poll() || restart_page[0]) restart_ui();
    }
    return 0;
}
