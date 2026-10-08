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
#include "display.h"
#include "fonts.h"
#include "home.h"
#include "input.h"
#include "layout.h"
#include "mpdc.h"
#include "notice.h"
#include "nowplaying.h"
#include "playthrough.h"
#include "settings.h"
#include "strings.h"
#include "theme.h"

#include <poll.h>
#include <stdio.h>
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

typedef enum { SCR_HOME, SCR_BROWSER, SCR_NOWPLAYING, SCR_SETTINGS, SCR_NOTICE } screen_t;

static lv_obj_t * browser_scr;
static screen_t cur = SCR_HOME;
static screen_t back_to = SCR_HOME;          /* where now-playing / Settings / a notice return to */
static char * argv0;
static char restart_page[32];                /* set by a setting that needs a fresh start */

static void leave_current(void)
{
    switch(cur) {
        case SCR_HOME:       home_hide(); break;
        case SCR_NOWPLAYING: nowplaying_hide(); break;
        case SCR_SETTINGS:   settings_hide(); break;
        case SCR_NOTICE:     notice_hide(); break;
        default: break;
    }
}

static void show_home(void)
{
    leave_current();
    cur = SCR_HOME;
    home_show();
}

static void show_browser(void)
{
    leave_current();
    cur = SCR_BROWSER;
    lv_screen_load(browser_scr);
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
        default:             show_home(); break;
    }
}

/* The Home page chose a view. */
static void open_view(home_view_t v)
{
    switch(v) {
        case HOME_FOLDERS:     show_browser(); break;
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

/* The browser started a track: show it. */
static void on_play(void)
{
    enter_nowplaying();
}

/* ---- action dispatch ---- */

static void dispatch(ui_action_t a)
{
    /* transport keys work on every screen */
    switch(a) {
        case ACT_PLAYPAUSE: mpd_toggle_pause(); break;
        case ACT_NEXT:      mpd_next();         break;
        case ACT_PREV:      mpd_previous();     break;
        /* Remote-only explicit play/pause (cec.c), consumed here so no screen
         * sees an action it does not know; now-playing catches up on its poll. */
        case ACT_PLAY:      mpd_resume();       return;
        case ACT_PAUSE:     mpd_pause();        return;
        case ACT_HOME:
            if(cur == SCR_NOWPLAYING) go_back(); else enter_nowplaying();
            return;
        case ACT_SETTINGS:
            switch(cur) {
                /* a toggle, like the now-playing button: press again, it goes */
                case SCR_BROWSER:    browser_toggle_picker(); break;       /* the view picker (0.15) */
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
    char * args[8];
    int n = 0;
    args[n++] = argv0;
    if(folder && folder[0]) { args[n++] = "--folder"; args[n++] = (char *)folder; }
    if(cur == SCR_NOWPLAYING) args[n++] = "--nowplaying";
    else if(cur == SCR_BROWSER) args[n++] = "--browser";
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
}

/* Hand MPD's change notices to whoever shows or acts on that state. */
static void mpd_changed(unsigned what)
{
    if(what & (MPD_CHANGED_DATABASE | MPD_CHANGED_UPDATE))
        browser_library_changed(what & MPD_CHANGED_DATABASE);
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
    if(mpd_idle_start()) mpd_changed(MPD_CHANGED_ALL);
}

int main(int argc, char ** argv)
{
    const char * resume_folder = NULL, * resume_settings = NULL;
    bool resume_nowplaying = false, resume_browser = false;
    argv0 = argv[0];
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--folder") == 0 && i + 1 < argc) resume_folder = argv[++i];
        else if(strcmp(argv[i], "--nowplaying") == 0) resume_nowplaying = true;
        else if(strcmp(argv[i], "--browser") == 0) resume_browser = true;
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

    mpd_connect();   /* may fail: browser shows "Starting library" and retries */
    mpd_idle_start();/* change notices; retried with the connection if MPD isn't up */
    browser_scr = lv_screen_active();
    browser_create(browser_scr, grp, on_play, show_home, resume_folder);
    nowplaying_create();
    settings_create(restart_for_setting, view_options_changed);
    notice_create();
    home_create(open_view);
    lv_timer_create(mpd_retry_cb, 2000, NULL);
    /* the screen we were on before a restart; Home otherwise */
    cur = SCR_HOME;
    home_show();
    if(resume_browser) show_browser();
    if(resume_nowplaying) enter_nowplaying();
    if(resume_settings) enter_settings(resume_settings, false);
    fprintf(stderr, "simpleton-ui: play-through folders: %s\n", playthrough_enabled() ? "on" : "off");
    playthrough_mpd_changed(MPD_CHANGED_PLAYER);   /* restarted mid-album: catch up now */

    for(;;) {
        uint32_t idle_ms = lv_timer_handler();
        if(idle_ms > 20) idle_ms = 20;          /* keep hold/repeat timing tight */

        struct pollfd pfd[4] = { { .fd = input_fd(), .events = POLLIN },
                                 { .fd = display_hotplug_fd(), .events = POLLIN },
                                 { .fd = cec_fd(), .events = POLLIN | POLLPRI },
                                 { .fd = mpd_idle_fd(), .events = POLLIN } };
        poll(pfd, 4, (int)idle_ms);             /* negative fds are ignored by poll() */

        input_poll();
        cec_poll();                             /* TV remote keys join the same queue */
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
