/*
 * SimpletonOS UI - entry point.
 *
 * Wiring: DRM display -> LVGL; joypad (evdev) -> abstract actions -> either
 * the LVGL keypad path (navigation, select) or app-level handlers (back,
 * transport). MPD is reached through mpdc.c. The UI must come up even when
 * MPD isn't running yet, and keeps retrying it in the background.
 *
 * DRM notes (verified against LVGL v9.5.0):
 *   - lv_linux_drm_set_file() does NOT auto-detect from NULL; use
 *     lv_linux_drm_find_device_path() explicitly.
 *   - lv_linux_drm_create() registers its own tick source.
 *   - connector_id -1 = first connector, native mode.
 *
 * Keypad notes: LVGL's keypad driver moves group focus only on LV_KEY_NEXT /
 * LV_KEY_PREV, so D-pad down/up are mapped to those; ENTER produces the
 * PRESSED/CLICKED pair on the focused row. Everything else bypasses LVGL.
 *
 * Screens: the browser lives on the default screen and uses the keypad
 * group; the now-playing screen is its own lv_obj screen and takes every
 * action directly (see nowplaying.c). main.c owns the switch between them:
 * playing a track or pressing Start enters now-playing, B (with the
 * controls already hidden) returns to the browser at the remembered row.
 */

#include "lvgl.h"
#include "src/drivers/display/drm/lv_linux_drm.h"

#include "browser.h"
#include "input.h"
#include "mpdc.h"
#include "nowplaying.h"
#include "theme.h"

#include <poll.h>
#include <stdio.h>
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
        case ACT_LEFT:      key_tap(LV_KEY_LEFT);  break;
        case ACT_RIGHT:     key_tap(LV_KEY_RIGHT); break;
        case ACT_SELECT:    key_tap(LV_KEY_ENTER); break;
        case ACT_PLAYPAUSE: case ACT_NEXT: case ACT_PREV: break;   /* handled above */
        case ACT_VOL_UP:
        case ACT_VOL_DOWN:  /* Fixed-volume mode: overlay comes with Settings work */ break;
        default:            browser_handle_action(a); break;
    }
}

static void mpd_retry_cb(lv_timer_t * t)
{
    (void)t;
    browser_tick();
}

int main(void)
{
    lv_init();

    lv_display_t * disp = lv_linux_drm_create();
    if(disp == NULL) { fprintf(stderr, "simpleton-ui: lv_linux_drm_create() failed\n"); return 1; }

    char * dev_path = lv_linux_drm_find_device_path();
    if(dev_path == NULL) { fprintf(stderr, "simpleton-ui: no DRM device found\n"); return 1; }
    lv_result_t res = lv_linux_drm_set_file(disp, dev_path, -1);
    lv_free(dev_path);
    if(res != LV_RESULT_OK) { fprintf(stderr, "simpleton-ui: lv_linux_drm_set_file() failed\n"); return 1; }

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

    mpd_connect();   /* may fail: browser shows "Starting library" and retries */
    browser_scr = lv_screen_active();
    browser_create(browser_scr, grp, enter_nowplaying);
    nowplaying_create();
    lv_timer_create(mpd_retry_cb, 2000, NULL);

    for(;;) {
        uint32_t idle_ms = lv_timer_handler();
        if(idle_ms > 20) idle_ms = 20;          /* keep hold/repeat timing tight */

        struct pollfd pfd = { .fd = input_fd(), .events = POLLIN };
        poll(&pfd, input_fd() >= 0 ? 1 : 0, (int)idle_ms);

        input_poll();
        for(ui_action_t a; (a = input_next_action()) != ACT_NONE;) dispatch(a);
    }
    return 0;
}
