/*
 * SimpletonOS UI - entry point.
 *
 * Wiring: DRM display -> LVGL; joypad (evdev) -> abstract actions -> either
 * the LVGL keypad path (navigation, select) or app-level handlers (back,
 * transport). MPD is reached through mpdc.c. The UI must come up even when
 * MPD isn't running yet, and keeps retrying it in the background.
 *
 * Display (29 Sep 2026): display.c picks the output (HDMI if connected,
 * else the panel, 1080p max on a TV) and sets ui_base to the square stage
 * every screen is laid out in. When HDMI is plugged or unplugged the UI
 * restarts itself on the new output (a fresh exec, well under a second;
 * playback is MPD's and never stops), carrying over which screen and which
 * folder was showing: --folder <uri> and --nowplaying.
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
 * Screens: the browser lives on the default screen and uses the keypad
 * group; the now-playing screen is its own lv_obj screen and takes every
 * action directly (see nowplaying.c). main.c owns the switch between them:
 * playing a track or pressing Start enters now-playing, B (with the
 * controls already hidden) returns to the browser at the remembered row.
 */

#include "lvgl.h"
#include "browser.h"
#include "cec.h"
#include "display.h"
#include "fonts.h"
#include "input.h"
#include "mpdc.h"
#include "nowplaying.h"
#include "playthrough.h"
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

static lv_obj_t * browser_scr;
static bool in_nowplaying;

static void enter_nowplaying(void)
{
    if(in_nowplaying) return;
    in_nowplaying = true;
    nowplaying_show();
}

static void leave_nowplaying(void)
{
    if(!in_nowplaying) return;
    in_nowplaying = false;
    nowplaying_hide();
    lv_screen_load(browser_scr);
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
            if(in_nowplaying) leave_nowplaying(); else enter_nowplaying();
            return;
        default: break;
    }

    if(in_nowplaying) {
        if(nowplaying_handle_action(a) == NP_EXIT) leave_nowplaying();
        return;
    }

    switch(a) {
        case ACT_UP:        key_tap(LV_KEY_PREV);  break;
        case ACT_DOWN:      key_tap(LV_KEY_NEXT);  break;
        case ACT_SELECT:    key_tap(LV_KEY_ENTER); break;
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV: break;   /* handled above */
        case ACT_VOL_UP:
        case ACT_VOL_DOWN:  /* Fixed-volume mode: overlay comes with Settings work */ break;
        default:            browser_handle_action(a); break;     /* back, left/right paging, menu */
    }
}

/* Same program, same screen and folder, new output. The DRM and uevent fds
 * are close-on-exec; anything else we opened (the MPD socket) is closed here
 * so a run of hotplugs can't pile up descriptors. */
static void restart_on_new_output(char * argv0)
{
    const char * folder = browser_current_uri();
    char * args[6];
    int n = 0;
    args[n++] = argv0;
    if(folder && folder[0]) { args[n++] = "--folder"; args[n++] = (char *)folder; }
    if(in_nowplaying) args[n++] = "--nowplaying";
    args[n] = NULL;
    for(int fd = 3; fd < 256; fd++) close(fd);
    execv("/proc/self/exe", args);
    _exit(1);                                   /* systemd brings us back */
}

/* Hand MPD's change notices to whoever shows or acts on that state. */
static void mpd_changed(unsigned what)
{
    if(what & (MPD_CHANGED_DATABASE | MPD_CHANGED_UPDATE))
        browser_library_changed(what & MPD_CHANGED_DATABASE);
    if(what & (MPD_CHANGED_PLAYER | MPD_CHANGED_MIXER | MPD_CHANGED_OPTIONS |
               MPD_CHANGED_PLAYLIST | MPD_CHANGED_OUTPUT))
        nowplaying_mpd_changed();
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
    const char * resume_folder = NULL;
    bool resume_nowplaying = false;
    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--folder") == 0 && i + 1 < argc) resume_folder = argv[++i];
        else if(strcmp(argv[i], "--nowplaying") == 0) resume_nowplaying = true;
    }

    lv_init();

    display_info_t di;
    lv_display_t * disp = display_init(&di);
    if(disp == NULL) { fprintf(stderr, "simpleton-ui: display init failed\n"); return 1; }
    ui_base = di.stage;
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
    browser_create(browser_scr, grp, enter_nowplaying, resume_folder);
    nowplaying_create();
    lv_timer_create(mpd_retry_cb, 2000, NULL);
    if(resume_nowplaying) enter_nowplaying();
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

        if(display_hotplug_poll()) restart_on_new_output(argv[0]);
    }
    return 0;
}
