/*
 * SimpletonOS UI - power (implementation). See power.h.
 */
#include "power.h"
#include "config.h"
#include "mpdc.h"
#include "strings.h"
#include "theme.h"
#include "tilestyle.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define POWER_KEY_NAME     "rk805 pwrkey"
#define BATTERY_PERIOD_S   5          /* gauge reads                         */
#define DIM_BEFORE_OFF_S   30         /* the screen dims this long before off */
#define RESUME_GUARD_MS    700        /* key events this soon after a wake are the wake itself */
#define TOAST_MS           5000
#define LOW_RED_PCT        15
#define LOW_NOTICE_PCT     10
#define LOW_NOTICE2_PCT    5
#define EMPTY_PCT          3
#define RED                0xE03030
#define INDICATOR_MARGIN   14         /* stage units from the top and right edges */
#define BLACKOUT_MS        1000       /* the screen fades to black this long before it goes off */

/* ------------------------------------------------------------ state */

static const char * sys_root;
static int    key_fd = -1;
static bool   key_down;
static uint32_t key_down_ms;
static bool   menu_shown;
static uint32_t resumed_ms;

static power_battery_t bat = { .percent = -1, .raw_percent = -1 };
static int    last_pct_dir;               /* +1 charging, -1 discharging: the smoothing direction */
static bool   external;
static bool   indicator_hidden;           /* now-playing's wish */
static bool   playing;
static bool   screen_off, dimmed;
static unsigned awake_mask;
static uint32_t last_activity_ms;
static int    saved_brightness = -1;
static bool   notified10, notified5, shutting_down;

/* settings */
static int  set_screen_off_s = 60;
static bool set_dim = true;
static int  set_idle_sleep_s = 600;
static int  set_sleep_off_s = 3600;      /* asleep this long -> a clean power-off (0.22; 0 = never) */
static int  set_sleep_timer_min;
static bool set_long_skip = true;
static int  set_long_min = 20;
static bool set_leds = true;
static bool set_keep_on_plugged = true;
static int  set_battery_show = 1;        /* 0 never, 1 for a moment after a button, 2 always (Ian, 12 Oct 21:59) */
static uint32_t ind_until_ms;             /* "after a button": shown until then */
static bool ind_yield;                    /* a screen has something in the corner (the grid's name bar) */
static bool ind_visible;                  /* what the fade left on screen */
#define IND_SHOW_MS   4000
#define IND_QUIET_MS  60000                /* a button after this long without one brings the disc up */
#define IND_FADE_MS   300
#define IND_LOW_PCT   15                   /* under this, on battery, the disc stays whatever the setting */

static uint32_t sleep_timer_end_ms;       /* 0 = none */
static bool   sleep_at_track_end;
static int    sleep_timer_song;

static lv_obj_t * indicator, * ind_label;
static lv_obj_t * menu, * menu_rows[3], * menu_count;
static int    menu_focus;
static lv_obj_t * overlay, * overlay_label;
static lv_obj_t * blackout;               /* a black sheet over everything, faded in before the screen goes off */
static bool fading_out;
static lv_obj_t * toast;
static lv_timer_t * tick_timer, * toast_timer;

static void indicator_refresh(void);
static void (*resume_cb)(void);
static void (*poweroff_cb)(void);

/* ------------------------------------------------------------ files */

/* LVGL's tick: CLOCK_MONOTONIC on the unit (display.c), a virtual clock in
 * the rig, so the harness can hold the key for "two seconds" in no time. */
static uint32_t now_ms(void)
{
    return lv_tick_get();
}

static void sys_path(char * out, size_t len, const char * rel)
{
    snprintf(out, len, "%s/%s", sys_root, rel);
}

static bool read_line(const char * rel, char * out, size_t len)
{
    char path[512];
    sys_path(path, sizeof(path), rel);
    FILE * f = fopen(path, "r");
    out[0] = 0;
    if(!f) return false;
    bool ok = fgets(out, (int)len, f) != NULL;
    fclose(f);
    size_t n = strlen(out);
    while(n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) out[--n] = 0;
    return ok;
}

static long read_long(const char * rel, long fallback)
{
    char v[64];
    if(!read_line(rel, v, sizeof(v)) || !v[0]) return fallback;
    return strtol(v, NULL, 10);
}

static bool write_str(const char * rel, const char * value)
{
    char path[512];
    sys_path(path, sizeof(path), rel);
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);   /* O_TRUNC: harmless on sysfs, right on the rig's plain files */
    if(fd < 0) return false;
    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    return n == (ssize_t)strlen(value);
}

static bool write_int(const char * rel, long v)
{
    char s[32];
    snprintf(s, sizeof(s), "%ld", v);
    return write_str(rel, s);
}

/* --------------------------------------------------------- settings */

static int read_setting_int(const char * name, int def)
{
    char v[32];
    config_read(name, v, sizeof(v), NULL);
    if(!v[0]) return def;
    return (int)strtol(v, NULL, 10);
}

void power_reload_settings(void)
{
    set_screen_off_s    = read_setting_int("screen_off", 60);
    set_dim             = config_read_bool("screen_dim", true);
    set_idle_sleep_s    = read_setting_int("idle_sleep", 600);
    set_sleep_off_s     = read_setting_int("sleep_off", 3600);
    set_long_skip       = config_read_bool("long_track_skip", true);
    set_long_min        = read_setting_int("long_track_min", 20);
    set_leds            = config_read_bool("leds", true);
    set_keep_on_plugged = config_read_bool("keep_on_plugged", true);
    set_battery_show    = read_setting_int("battery_show", 1);
    if(indicator) indicator_refresh();
    int timer = read_setting_int("sleep_timer", 0);
    if(timer != set_sleep_timer_min) {
        set_sleep_timer_min = timer;
        sleep_at_track_end = false;
        sleep_timer_end_ms = timer > 0 ? now_ms() + (uint32_t)timer * 60000u : 0;
        if(timer > 0) fprintf(stderr, "simpleton-ui: power: sleep timer %d min\n", timer);
    }
    fprintf(stderr, "simpleton-ui: power: screen off %d s%s, idle sleep %d s, off after %d s asleep, long tracks %s from %d min, lights %s, plugged keeps the screen %s, indicator %s\n",
            set_screen_off_s, set_dim ? " (dims first)" : "", set_idle_sleep_s, set_sleep_off_s,
            set_long_skip ? "skip 30 s" : "skip whole", set_long_min, set_leds ? "on" : "off",
            set_keep_on_plugged ? "on" : "off", set_battery_show >= 2 ? "always" : set_battery_show == 1 ? "after a button" : "never");
}

/* ---------------------------------------------------------- battery */

static void leds_apply(void)
{
    bool red = set_leds && bat.plugged && !bat.full;
    bool green = set_leds && bat.plugged && bat.full;
    write_int("class/leds/red:charging/brightness", red ? 255 : 0);
    write_int("class/leds/green:status/brightness", green ? 255 : 0);
}

/* The disc (Ian, 12 Oct): green on the cable, else blue / yellow / red by
 * charge; the number in the ink that reads better on it. */
static battery_style_t ind_style;         /* the style the disc was built with (the theme loads after power_init) */

static void indicator_apply_style(const battery_style_t * st)
{
    ind_style = *st;
    lv_obj_set_size(indicator, PX(st->diameter), PX(st->diameter));
    lv_obj_align(indicator, LV_ALIGN_TOP_LEFT, PX(INDICATOR_MARGIN), PX(INDICATOR_MARGIN));   /* the left: nobody puts the battery there (Ian) */
    const lv_font_t * nf = ui_font_face_px(st->font, PX(st->size), false);
    lv_obj_set_style_text_font(ind_label, nf ? nf : ui_font(st->size), 0);
    lv_obj_center(ind_label);
}

static void indicator_set_visible(bool on)
{
    if(on == ind_visible) return;
    ind_visible = on;
    lv_anim_delete(indicator, NULL);
    if(on) { lv_obj_remove_flag(indicator, LV_OBJ_FLAG_HIDDEN); lv_obj_fade_in(indicator, IND_FADE_MS, 0); }
    else lv_obj_fade_out(indicator, IND_FADE_MS, 0);     /* stays an object at opacity 0 */
}

/* When the disc shows: always / for IND_SHOW_MS after a button / never - but
 * low on battery it stays, and it steps aside for a screen's own corner. */
static bool indicator_wanted(void)
{
    if(ind_yield || bat.percent < 0 || (external && bat.plugged)) return false;
    bool low = !bat.plugged && bat.percent < IND_LOW_PCT;
    bool moment = ind_until_ms && (int32_t)(ind_until_ms - now_ms()) > 0;
    if(set_battery_show == 1) return moment || (low && !indicator_hidden);   /* a moment shows even over the full-screen cover */
    if(indicator_hidden) return false;
    return set_battery_show >= 2 || low;
}

static void indicator_refresh(void)
{
    if(!indicator) return;
    if(!indicator_wanted()) { indicator_set_visible(false); return; }
    const battery_style_t * st = tilestyle_battery();
    if(memcmp(st, &ind_style, sizeof(ind_style)) != 0) indicator_apply_style(st);
    uint32_t disc = bat.plugged ? st->charge
                  : bat.percent >= st->high_from ? st->high
                  : bat.percent >= st->mid_from  ? st->mid : st->low;
    uint32_t ink = tilestyle_contrast(disc, st->ink) >= tilestyle_contrast(disc, st->ink_light) ? st->ink : st->ink_light;
    char txt[8];
    snprintf(txt, sizeof(txt), "%02d", bat.percent);
    lv_label_set_text(ind_label, txt);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(disc), 0);
    lv_obj_set_style_text_color(ind_label, lv_color_hex(ink), 0);
    indicator_set_visible(true);
}

/* A moment of the disc: a button, a plug, a notice. */
static void indicator_moment(void)
{
    ind_until_ms = now_ms() + IND_SHOW_MS;
    if(!ind_until_ms) ind_until_ms = 1;
    indicator_refresh();
}

void power_indicator_yield(bool yield)
{
    ind_yield = yield;
    indicator_refresh();
}

static void toast_hide_cb(lv_timer_t * t)
{
    (void)t;
    if(toast) lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
    toast_timer = NULL;
}

static void toast_show(const char * text)
{
    indicator_moment();
    if(!toast) return;
    lv_label_set_text(toast, text);
    lv_obj_remove_flag(toast, LV_OBJ_FLAG_HIDDEN);
    if(toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(toast_hide_cb, TOAST_MS, NULL);
    lv_timer_set_repeat_count(toast_timer, 1);
}

static void battery_read(void)
{
    char status[32];
    long cap = read_long("class/power_supply/battery/capacity", -1);
    read_line("class/power_supply/battery/status", status, sizeof(status));
    long online = read_long("class/power_supply/charger/online", 0);
    bat.charge_now_uah = read_long("class/power_supply/battery/charge_now", 0);
    bat.current_ua     = read_long("class/power_supply/battery/current_avg", 0);
    bat.raw_percent = (int)cap;
    bool was_plugged = bat.plugged;
    bat.plugged  = online == 1;
    bat.charging = bat.plugged && strcmp(status, "Charging") == 0;
    bat.full     = bat.plugged && (strcmp(status, "Full") == 0 || cap >= 100);

    /* smoothing: the shown number never rises on battery, never falls on the
     * cable; a change of direction (plug, unplug) lets it jump once */
    int dir = bat.plugged ? 1 : -1;
    if(cap >= 0) {
        if(bat.percent < 0 || dir != last_pct_dir) bat.percent = (int)cap;
        else if(dir > 0 && cap > bat.percent) bat.percent = (int)cap;
        else if(dir < 0 && cap < bat.percent) bat.percent = (int)cap;
        last_pct_dir = dir;
    }
    if(was_plugged != bat.plugged) {
        notified10 = notified5 = false;
        fprintf(stderr, "simpleton-ui: power: %s (%d%%)\n", bat.plugged ? "on the cable" : "on battery", bat.raw_percent);
        indicator_moment();
    }
    indicator_refresh();
    leds_apply();

    /* low battery, on battery only */
    if(!bat.plugged && bat.percent >= 0) {
        char txt[96];
        if(bat.percent <= EMPTY_PCT && !shutting_down) {
            fprintf(stderr, "simpleton-ui: power: battery empty (%d%%), shutting down\n", bat.raw_percent);
            toast_show(T(S_BATTERY_EMPTY));
            lv_refr_now(NULL);
            sleep(3);
            power_poweroff();
            return;
        }
        if(bat.percent <= LOW_NOTICE2_PCT && !notified5) {
            notified5 = notified10 = true;
            char n[8]; snprintf(n, sizeof(n), "%d", bat.percent);
            ui_strf(txt, sizeof(txt), S_BATTERY_LOW, "n", n, NULL);
            toast_show(txt);
        }
        else if(bat.percent <= LOW_NOTICE_PCT && !notified10) {
            notified10 = true;
            char n[8]; snprintf(n, sizeof(n), "%d", bat.percent);
            ui_strf(txt, sizeof(txt), S_BATTERY_LOW, "n", n, NULL);
            toast_show(txt);
        }
    }
}

const power_battery_t * power_battery(void) { return &bat; }

void power_battery_text(char * out, int len)
{
    if(bat.percent < 0) { snprintf(out, (size_t)len, "%s", T(S_BATTERY_UNKNOWN)); return; }
    const char * state = bat.full ? T(S_BATTERY_FULL) : bat.plugged ? T(S_BATTERY_CHARGING) : T(S_BATTERY_ON_BATTERY);
    snprintf(out, (size_t)len, "%d%%, %s", bat.percent, state);
}

/* ----------------------------------------------------------- screen */

/* On a TV the panel is not ours to touch (it is dark already, the TV shows
 * the picture): no dimming, no screen-off, no backlight writes. */
static void backlight_set_power(bool on)
{
    if(external) return;
    write_int("class/backlight/backlight/bl_power", on ? 0 : 4);
}

/* The undimmed brightness is kept in a settings file while dimmed, so a
 * restart of the UI (a crash, an HDMI plug) in the dim state puts it back. */
static void screen_dim(void)
{
    if(dimmed || screen_off || external) return;
    dimmed = true;
    long cur = read_long("class/backlight/backlight/brightness", -1);
    if(cur > 0) saved_brightness = (int)cur;
    if(saved_brightness > 0) {
        char v[16];
        snprintf(v, sizeof(v), "%d", saved_brightness);
        config_write("screen_brightness_saved", v);
        write_int("class/backlight/backlight/brightness", saved_brightness / 4 > 8 ? saved_brightness / 4 : 8);
    }
}

static void screen_undim(void)
{
    if(!dimmed) return;
    dimmed = false;
    if(saved_brightness > 0) write_int("class/backlight/backlight/brightness", saved_brightness);
    config_write("screen_brightness_saved", "");
}

/* The black sheet: the panel shows garbage for a moment as the backlight
 * goes (Ian, 12 Oct: "glitchy"), so the picture fades to black first and
 * the backlight goes on a black frame; on the way back the backlight comes
 * up on black too and the picture is then uncovered. */
static void blackout_anim_cb(void * obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }

static void blackout_begin(void)
{
    if(fading_out || !blackout) return;
    fading_out = true;
    lv_obj_set_style_bg_opa(blackout, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(blackout, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(blackout);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, blackout);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, BLACKOUT_MS * 7 / 10);
    lv_anim_set_exec_cb(&a, blackout_anim_cb);
    lv_anim_start(&a);
}

static void blackout_end(void)
{
    fading_out = false;
    if(!blackout) return;
    lv_anim_delete(blackout, blackout_anim_cb);
    lv_obj_add_flag(blackout, LV_OBJ_FLAG_HIDDEN);
}

static void screen_set_off(bool off)
{
    if(off == screen_off) return;
    if(off && external) return;
    screen_off = off;
    if(off) {
        screen_undim();
        if(blackout) {                                   /* a black frame under the backlight going */
            lv_anim_delete(blackout, blackout_anim_cb);
            lv_obj_set_style_bg_opa(blackout, LV_OPA_COVER, 0);
            lv_obj_remove_flag(blackout, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(blackout);
            lv_refr_now(NULL);
        }
        backlight_set_power(false);
        lv_display_enable_invalidation(NULL, false);     /* nothing is drawn while it's dark */
        fprintf(stderr, "simpleton-ui: power: screen off\n");
    }
    else {
        lv_display_enable_invalidation(NULL, true);
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
        if(blackout && !external) {                      /* the backlight comes up on black, then the picture */
            lv_obj_set_style_bg_opa(blackout, LV_OPA_COVER, 0);
            lv_obj_remove_flag(blackout, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(blackout);
            lv_refr_now(NULL);
        }
        backlight_set_power(true);
        blackout_end();
        fprintf(stderr, "simpleton-ui: power: screen on\n");
    }
}

bool power_screen_off(void) { return screen_off; }

static bool keep_awake(void)
{
    if(awake_mask & ~POWER_AWAKE_PLUGGED_PLAYING) return true;
    return set_keep_on_plugged && bat.plugged && playing;
}

void power_keep_awake(unsigned holder, bool on)
{
    if(on) awake_mask |= holder; else awake_mask &= ~holder;
    if(on) { screen_undim(); if(!screen_off && fading_out) blackout_end(); screen_set_off(false); }
}

static void activity_now(void)
{
    last_activity_ms = now_ms();
    screen_undim();
    if(!screen_off && fading_out) blackout_end();        /* a press during the fade: back to the picture */
    screen_set_off(false);
}

bool power_activity(ui_action_t a)
{
    bool was_off = screen_off;
    /* the disc comes up on the first button after a quiet minute (or on the
     * one that lights the screen), not on every press while browsing (Ian,
     * 12 Oct 22:34: "you start browsing and it quickly fades away") */
    if(was_off || now_ms() - last_activity_ms >= IND_QUIET_MS) indicator_moment();
    bool transport = a == ACT_PLAYPAUSE || a == ACT_NEXT || a == ACT_PREV || a == ACT_PLAY || a == ACT_PAUSE;
    if(was_off && transport) {
        /* X and Y work in the dark and leave it dark (Ian, 12 Oct) */
        last_activity_ms = now_ms();
        return false;
    }
    activity_now();
    return was_off;        /* the press that lit the screen does nothing else */
}

/* ------------------------------------------------------------ sleep */

static void overlay_show(const char * text)
{
    if(!overlay) return;
    lv_label_set_text(overlay_label, text);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    lv_refr_now(NULL);
}

static void overlay_hide(void)
{
    if(overlay) lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

static void drain_key(void)
{
    if(key_fd < 0) return;
    struct input_event ev[16];
    while(read(key_fd, ev, sizeof(ev)) > 0) {}
}

/* ------------------------------------------------ sleep, then off (0.22) */

/* Ian, 16 Oct: a unit asleep looks off, and asleep it lost 35 % in 17.5 h -
 * people would find it dead. So every sleep (the key, the menu, the idle
 * sleep, the sleep timer) sets the PMIC's clock (rk808-rtc) to wake the
 * unit after sleep_off seconds; woken by that alarm rather than a button,
 * it powers off cleanly, in the dark, with the music paused. */
static long boottime_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);         /* counts the time asleep, unlike MONOTONIC */
    return (long)ts.tv_sec;
}

static bool alarm_set(int seconds)
{
    write_str("class/rtc/rtc0/device/power/wakeup", "enabled");   /* the RTC may wake us */
    write_str("class/rtc/rtc0/wakealarm", "0");                     /* a new one needs the old cleared */
    char v[24];
    snprintf(v, sizeof(v), "+%d", seconds);
    bool ok = write_str("class/rtc/rtc0/wakealarm", v);
    if(ok) fprintf(stderr, "simpleton-ui: power: off in %d s unless woken\n", seconds);
    else fprintf(stderr, "simpleton-ui: power: couldn't set the wake alarm (%s): this sleep won't turn into off\n", strerror(errno));
    return ok;
}

static bool quiet_off;                    /* power off without lighting the screen */

void power_sleep(void)
{
    if(menu_shown) { lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN); menu_shown = false; }
    key_down = false;
    /* "Sleeping..." on a lit screen; a dark one stays dark (the idle sleep) */
    if(!screen_off) { screen_undim(); overlay_show(T(S_POWER_SLEEPING)); }
    fprintf(stderr, "simpleton-ui: power: sleeping\n");
    bool alarm = set_sleep_off_s > 0 && alarm_set(set_sleep_off_s);
    long t0 = boottime_s();
    /* deep suspend: this write returns when the unit wakes */
    if(!write_str("power/state", "mem")) fprintf(stderr, "simpleton-ui: power: suspend failed: %s\n", strerror(errno));
    resumed_ms = now_ms();
    drain_key();
    long slept = boottime_s() - t0;
    const char * test_slept = getenv("SIMPLETON_TEST_SLEPT_S");   /* the rig: "the unit slept this long" */
    if(test_slept && test_slept[0]) slept += atol(test_slept);
    if(alarm) {
        /* the alarm fired (the RTC clears it) or the time is up: off */
        char left[32];
        bool fired = read_line("class/rtc/rtc0/wakealarm", left, sizeof(left)) && left[0] == 0;
        if(fired || slept >= set_sleep_off_s - 5) {
            fprintf(stderr, "simpleton-ui: power: asleep %ld s: off\n", slept);
            mpd_pause();                  /* the next start comes up paused where it was */
            overlay_hide();
            screen_set_off(true);         /* the panel woke with the unit: dark again */
            quiet_off = true;
            power_poweroff();
            if(shutting_down) return;     /* systemd is taking it down */
            quiet_off = false;            /* the rig's stand-in command: carry on awake */
        }
        else write_str("class/rtc/rtc0/wakealarm", "0");   /* woken by a button: no alarm left behind */
    }
    overlay_hide();
    screen_set_off(false);
    fprintf(stderr, "simpleton-ui: power: awake\n");
    if(sleep_timer_end_ms || sleep_at_track_end) { config_write("sleep_timer", "0"); set_sleep_timer_min = 0; }   /* one-shot: a sleep spends it */
    sleep_timer_end_ms = 0;
    sleep_at_track_end = false;
    activity_now();
    battery_read();
    indicator_moment();                   /* back from sleep: the disc for a moment */
    if(resume_cb) resume_cb();
}

void power_poweroff(void)
{
    if(shutting_down) return;                  /* once is enough: systemd is on it */
    shutting_down = true;
    key_down = false;
    if(menu_shown) { lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN); menu_shown = false; }
    if(!quiet_off) {                           /* off from a sleep stays dark */
        screen_undim();
        screen_set_off(false);
        overlay_show(T(S_POWER_SHUTTING_DOWN));
    }
    fprintf(stderr, "simpleton-ui: power: powering off\n");
    if(poweroff_cb) poweroff_cb();
    const char * cmd = getenv("SIMPLETON_POWEROFF_CMD");
    int rc = system(cmd && cmd[0] ? cmd : "systemctl poweroff");
    if(rc != 0) fprintf(stderr, "simpleton-ui: power: poweroff command returned %d\n", rc);
    if(cmd && cmd[0]) { overlay_hide(); shutting_down = false; }   /* a test: carry on */
}

/* ------------------------------------------------------------- menu */

static void menu_render(void)
{
    for(int i = 0; i < 3; i++) {
        bool f = i == menu_focus;
        lv_obj_set_style_bg_opa(menu_rows[i], f ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(menu_rows[i], lv_color_hex(f ? UI_COLOR_FOCUS_FG : UI_COLOR_FG), 0);
    }
}

static void menu_show(void)
{
    if(!menu) return;
    menu_focus = 0;
    menu_render();
    lv_label_set_text(menu_count, "");
    lv_obj_remove_flag(menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(menu);
    menu_shown = true;
    activity_now();
}

static void menu_hide(void)
{
    if(menu) lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    menu_shown = false;
}

bool power_menu_active(void) { return menu_shown; }

void power_menu_handle_action(ui_action_t a)
{
    switch(a) {
        case ACT_UP:   menu_focus = (menu_focus + 2) % 3; menu_render(); break;
        case ACT_DOWN: menu_focus = (menu_focus + 1) % 3; menu_render(); break;
        case ACT_BACK: menu_hide(); break;
        case ACT_SELECT:
            if(menu_focus == 0) power_sleep();
            else if(menu_focus == 1) power_poweroff();
            else menu_hide();
            break;
        default: break;
    }
}

/* -------------------------------------------------------- power key */

static int open_power_key(void)
{
    DIR * d = opendir("/dev/input");
    if(!d) return -1;
    struct dirent * e;
    int found = -1;
    while(found < 0 && (e = readdir(d)) != NULL) {
        if(strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300], name[64] = "";
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        if(ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 && strcmp(name, POWER_KEY_NAME) == 0) {
            /* ours alone: logind stops acting on it while we hold the grab */
            if(ioctl(fd, EVIOCGRAB, 1) != 0) fprintf(stderr, "simpleton-ui: power: cannot grab %s: %s\n", path, strerror(errno));
            fprintf(stderr, "simpleton-ui: power: power key is %s\n", path);
            found = fd;
        }
        else close(fd);
    }
    closedir(d);
    if(found < 0) fprintf(stderr, "simpleton-ui: power: no \"%s\" device; the power key stays with logind\n", POWER_KEY_NAME);
    return found;
}

int power_key_fd(void) { return key_fd; }

void power_key_event(int value)
{
    uint32_t t = now_ms();
    if(resumed_ms && t - resumed_ms < RESUME_GUARD_MS) return;   /* the press that woke us */
    if(value == 1) {
        if(key_down) return;
        key_down = true;
        key_down_ms = t;
        activity_now();                       /* the screen lights, so the menu can show */
        return;
    }
    if(value != 0 || !key_down) return;
    key_down = false;
    if(menu_shown) { lv_label_set_text(menu_count, ""); return; }   /* released on the menu: it stays */
    if(t - key_down_ms < POWER_HOLD_MENU_MS) {
        if(shutting_down) return;
        power_sleep();                                               /* a short press: sleep (Ian, 12 Oct) */
    }
}

void power_poll(void)
{
    if(key_fd >= 0) {
        struct input_event ev[8];
        ssize_t n;
        while((n = read(key_fd, ev, sizeof(ev))) > 0) {
            int count = (int)(n / sizeof(ev[0]));
            for(int i = 0; i < count; i++)
                if(ev[i].type == EV_KEY && ev[i].code == KEY_POWER) power_key_event(ev[i].value);
        }
    }
    if(!key_down) return;
    uint32_t held = now_ms() - key_down_ms;
    if(held >= POWER_HOLD_MENU_MS && !menu_shown) menu_show();
    if(menu_shown) {
        if(held >= POWER_HOLD_OFF_MS) { power_poweroff(); return; }
        char txt[64], n[8];
        int left = (int)((POWER_HOLD_OFF_MS - held + 999) / 1000);
        snprintf(n, sizeof(n), "%d", left);
        ui_strf(txt, sizeof(txt), S_POWER_OFF_IN, "n", n, NULL);
        lv_label_set_text(menu_count, txt);
    }
}

/* ------------------------------------------------------------- tick */

static void tick_cb(lv_timer_t * t)
{
    (void)t;
    static int n;
    uint32_t now = now_ms();

    if(++n % BATTERY_PERIOD_S == 0 || bat.percent < 0) {
        battery_read();
        mpd_status_t st;
        bool was = playing;
        if(mpd_status(&st)) {
            playing = strcmp(st.state, "play") == 0;
            if(sleep_at_track_end && st.songid != sleep_timer_song) {
                /* the timer ran out during the last track: it is over now */
                mpd_pause();
                sleep_at_track_end = false;
                power_sleep();
                return;
            }
        }
        if(was != playing) indicator_refresh();
    }
    if(shutting_down) return;
    if(ind_visible && set_battery_show == 1) indicator_refresh();     /* the moment may be over */

    /* the sleep timer */
    if(sleep_timer_end_ms && (int32_t)(now - sleep_timer_end_ms) >= 0) {
        sleep_timer_end_ms = 0;
        config_write("sleep_timer", "0");            /* one-shot */
        set_sleep_timer_min = 0;
        mpd_status_t st;
        if(playing && mpd_status(&st) && st.songid >= 0) {
            sleep_at_track_end = true;                 /* finish this track first */
            sleep_timer_song = st.songid;
            fprintf(stderr, "simpleton-ui: power: sleep timer done, sleeping after this track\n");
        }
        else { power_sleep(); return; }
    }

    /* the screen */
    uint32_t quiet = now - last_activity_ms;
    if(set_screen_off_s > 0 && !keep_awake() && !menu_shown) {
        uint32_t off_ms = (uint32_t)set_screen_off_s * 1000u;
        uint32_t dim_ms = off_ms > 2u * DIM_BEFORE_OFF_S * 1000u ? off_ms - DIM_BEFORE_OFF_S * 1000u : off_ms / 2;
        if(quiet >= off_ms) screen_set_off(true);
        else if(quiet + BLACKOUT_MS >= off_ms) blackout_begin();
        else if(set_dim && quiet >= dim_ms) screen_dim();
    }
    else if(keep_awake()) { screen_undim(); if(!screen_off && fading_out) blackout_end(); screen_set_off(false); }

    /* idle: nothing playing, no buttons */
    if(set_idle_sleep_s > 0 && !playing && !menu_shown && quiet >= (uint32_t)set_idle_sleep_s * 1000u) {
        fprintf(stderr, "simpleton-ui: power: idle for %d s with nothing playing\n", set_idle_sleep_s);
        power_sleep();
    }
}

/* ------------------------------------------------------- long tracks */

bool power_skip_is_seek(void)
{
    if(!set_long_skip) return false;
    mpd_status_t st;
    if(!mpd_status(&st) || st.songid < 0) return false;
    return st.duration >= (float)set_long_min * 60.0f;
}

/* ----------------------------------------------------------- public */

void power_indicator_set_hidden(bool hidden)
{
    indicator_hidden = hidden;
    indicator_refresh();
}

void power_set_external(bool ext)
{
    external = ext;
    indicator_refresh();
}

static lv_obj_t * make_box(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, PX(10), 0);
    lv_obj_set_style_pad_all(o, PX(8), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void power_init(void (*on_resume)(void), void (*on_poweroff)(void))
{
    resume_cb = on_resume;
    poweroff_cb = on_poweroff;
    const char * env = getenv("SIMPLETON_SYS");
    sys_root = env && env[0] ? env : "/sys";
    last_activity_ms = now_ms() - IND_QUIET_MS;   /* the first button after a start brings the disc up */
    power_reload_settings();

    lv_obj_t * top = lv_layer_top();

    /* the indicator: a solid disc top right, the number centred in it */
    const battery_style_t * st = tilestyle_battery();
    indicator = lv_obj_create(top);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_set_style_pad_all(indicator, 0, 0);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    ind_label = lv_label_create(indicator);
    lv_label_set_text(ind_label, "");
    indicator_apply_style(st);
    lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);

    /* the black sheet (screen off) */
    blackout = lv_obj_create(top);
    lv_obj_set_size(blackout, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(blackout, 0, 0);
    lv_obj_set_style_border_width(blackout, 0, 0);
    lv_obj_set_style_radius(blackout, 0, 0);
    lv_obj_set_style_bg_color(blackout, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blackout, LV_OPA_COVER, 0);
    lv_obj_remove_flag(blackout, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(blackout, LV_OBJ_FLAG_HIDDEN);

    /* the toast: bottom centre */
    toast = lv_label_create(top);
    lv_obj_set_style_text_font(toast, UI_FONT_LIST, 0);
    lv_obj_set_style_text_color(toast, lv_color_hex(UI_COLOR_FOCUS_FG), 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(RED), 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(toast, PX(10), 0);
    lv_obj_set_style_pad_all(toast, PX(12), 0);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -PX(40));
    lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

    /* the full-screen overlay: "Sleeping..." / "Shutting down..." */
    overlay = lv_obj_create(top);
    lv_obj_set_size(overlay, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    overlay_label = lv_label_create(overlay);
    lv_obj_set_style_text_font(overlay_label, ui_font(36), 0);
    lv_obj_set_style_text_color(overlay_label, lv_color_hex(UI_COLOR_FG), 0);
    lv_obj_center(overlay_label);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    /* the power menu */
    menu = lv_obj_create(top);
    lv_obj_set_size(menu, ui_screen.w, ui_screen.h);
    lv_obj_set_pos(menu, 0, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_radius(menu, 0, 0);
    lv_obj_set_style_bg_color(menu, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_80, 0);
    lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t * box = make_box(menu);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_COLOR_DIM), 0);
    int row_h = PX(56), w = PX(360);
    lv_obj_set_size(box, w, row_h * 5 + PX(16));
    lv_obj_center(box);
    lv_obj_t * title = lv_label_create(box);
    lv_label_set_text(title, T(S_POWER_TITLE));
    lv_obj_set_style_text_font(title, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PX(12), PX(6));
    static const int ids[3] = { S_POWER_SLEEP, S_POWER_OFF, S_POWER_CANCEL };
    for(int i = 0; i < 3; i++) {
        lv_obj_t * r = lv_label_create(box);
        lv_label_set_text(r, T(ids[i]));
        lv_obj_set_style_text_font(r, UI_FONT_LIST, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_COLOR_FOCUS_BG), 0);
        lv_obj_set_style_radius(r, PX(6), 0);
        lv_obj_set_style_pad_all(r, PX(10), 0);
        lv_obj_set_width(r, w - PX(32));
        lv_obj_align(r, LV_ALIGN_TOP_LEFT, 0, row_h * (i + 1) - PX(8));
        menu_rows[i] = r;
    }
    menu_count = lv_label_create(box);
    lv_label_set_text(menu_count, "");
    lv_obj_set_style_text_font(menu_count, UI_FONT_HINT, 0);
    lv_obj_set_style_text_color(menu_count, lv_color_hex(RED), 0);
    lv_obj_align(menu_count, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(menu, LV_OBJ_FLAG_HIDDEN);
    menu_render();

    /* a fresh start finds the screen as the last run left it: light it,
     * and put back a brightness a dim had lowered */
    char saved[16];
    config_read("screen_brightness_saved", saved, sizeof(saved), NULL);
    if(saved[0] && atoi(saved) > 0) {
        write_int("class/backlight/backlight/brightness", atoi(saved));
        config_write("screen_brightness_saved", "");
    }
    backlight_set_power(true);

    key_fd = open_power_key();
    battery_read();
    tick_timer = lv_timer_create(tick_cb, 1000, NULL);
}
