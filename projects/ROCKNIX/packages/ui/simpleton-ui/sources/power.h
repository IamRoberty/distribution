/*
 * SimpletonOS UI - power: the battery, the screen, sleep and the power key
 * (0.18, 12 Oct 2026; power-management-proposal.md).
 *
 * What lives here:
 *
 *   The battery indicator  icon + percentage on the top layer, so every
 *                          screen has it without knowing (Ian, 28 Sep: always
 *                          on). The bolt follows the charger line (instant);
 *                          the percentage is smoothed - never up while on
 *                          battery, never down while on the cable - because
 *                          the gauge jumps under a heavy load (11 Oct: 62 ->
 *                          44 in 36 s). Red under 15 %; a one-time notice at
 *                          10 % and 5 %; a clean shutdown at 3 %. On a TV only
 *                          while on battery.
 *
 *   The screen             dims, then goes off after a while without a
 *                          button (bl_power, and LVGL stops drawing). X and
 *                          Y keep working in the dark (pause, skip); any
 *                          other button only lights the screen (Ian, 12 Oct).
 *                          "Keep awake" holders stop the timer: now-playing
 *                          while plugged in (a setting), visualisers later.
 *
 *   Sleep                  deep suspend, written to /sys/power/state by us
 *                          (not through systemd-sleep: ROCKNIX's hook took
 *                          13-15 s to get there). "Sleeping..." shows first.
 *                          After a while with nothing playing and no buttons
 *                          the unit sleeps by itself; a one-shot sleep timer
 *                          finishes the track, pauses, then sleeps.
 *
 *   The power key          grabbed from the kernel (EVIOCGRAB), so logind no
 *                          longer acts on it. Short press: sleep. Held: at
 *                          POWER_HOLD_MENU_MS the Sleep / Power off / Cancel
 *                          menu with a countdown that, if the key stays held,
 *                          powers off cleanly before the PMIC's own hard
 *                          cut-off. If the UI isn't running the grab is gone
 *                          and logind's suspend applies - a fine fallback.
 *
 *   Lights                 red while charging, green when full on the cable,
 *                          otherwise off; a setting turns them off.
 *
 * Paths come from $SIMPLETON_SYS (default /sys) so the rig can fake a
 * battery, a backlight and a power state file; $SIMPLETON_POWEROFF_CMD
 * replaces "systemctl poweroff" in tests. No key device in the rig: the
 * harness feeds presses with power_key_event().
 *
 * Settings (files in the settings folder; the Settings -> Power page writes
 * them, power_reload_settings() re-reads them):
 *   screen_off      seconds without a button before the screen goes off; 0 = never   (60)
 *   screen_dim      1 = dim before off                                                 (1)
 *   idle_sleep      seconds with nothing playing and no button before sleep; 0 = never (600)
 *   sleep_timer     minutes, one-shot: 0 = off                                         (0)
 *   long_track_skip 1 = X skips 30 s on a long track instead of the whole track        (1)
 *   long_track_min  minutes from which a track is long                                 (20)
 *   leds            1 = the two lights work                                            (1)
 *   keep_on_plugged 1 = the screen stays on while playing on the cable                 (1)
 *   battery_show    0 = never, 1 = for a few seconds on the first button after a quiet
 *                   minute, on waking the screen or the unit, 2 = always              (1)
 *                   (under 15 % on battery it stays whatever this says)
 */
#ifndef SIMPLETON_POWER_H
#define SIMPLETON_POWER_H

#include "input.h"
#include <stdbool.h>
#include <stdint.h>

/* Timing of the power key, ms. The PMIC cuts the power on its own after a
 * long hold (several seconds, to be measured on the unit); the countdown
 * must power off cleanly before that. */
#define POWER_HOLD_MENU_MS   2000   /* held this long: the menu comes up           */
#define POWER_HOLD_OFF_MS    7000   /* still held this long: clean power-off       */

/* Keep-awake holders (bit flags). */
enum {
    POWER_AWAKE_PLUGGED_PLAYING = 1 << 0,   /* set by power.c itself from the setting */
    POWER_AWAKE_VISUALIZER      = 1 << 1,   /* for the visualisations, when they exist */
    POWER_AWAKE_TEST            = 1 << 2,
};

typedef struct {
    int  percent;          /* -1 when unknown; the smoothed value shown            */
    int  raw_percent;      /* what the gauge says now                              */
    bool plugged;          /* the charger line                                     */
    bool charging;         /* plugged and the gauge says Charging                  */
    bool full;             /* plugged and the gauge says Full                      */
    long charge_now_uah;   /* the coulomb counter, uAh (0 when unknown)            */
    long current_ua;       /* +charging / -discharging, uA (0 when unknown)        */
} power_battery_t;

/* Create the top-layer pieces, read the settings, read the battery once,
 * open and grab the power key. `on_resume` runs after a sleep (the screens
 * refresh what may have changed), `on_poweroff` just before a clean
 * power-off (save state). Either may be NULL. */
void power_init(void (*on_resume)(void), void (*on_poweroff)(void));

/* The power key's fd for poll(), or -1. */
int power_key_fd(void);

/* Call once per loop: reads the key, times holds and the countdown. */
void power_poll(void);

/* Feed a key event in tests (1 = down, 0 = up), as if from the device. */
void power_key_event(int value);

/* A button was pressed (any producer). Resets the screen and idle timers.
 * Returns true when the press was spent waking the screen: the caller must
 * not act on it. X and Y (transport) are never spent: they work in the dark. */
bool power_activity(ui_action_t a);

/* The menu takes every action while it is up. */
bool power_menu_active(void);
void power_menu_handle_action(ui_action_t a);

/* True while the screen is off (the backlight down, drawing stopped). */
bool power_screen_off(void);

/* Keep the screen awake while `on` (a visualiser on screen, say). */
void power_keep_awake(unsigned holder, bool on);

/* Sleep now (the "Sleeping..." message, then deep suspend; returns after the
 * wake), or power off cleanly. */
void power_sleep(void);
void power_poweroff(void);

/* The screen on and what it shows. Now-playing hides the indicator while
 * its controls are down (the cover is the whole screen then). */
void power_indicator_set_hidden(bool hidden);

/* A screen has something of its own in the indicator's corner (the grid's
 * name bar): the disc steps aside while `yield` is true. */
void power_indicator_yield(bool yield);

/* The TV (external screen): the indicator shows only on battery, and the
 * panel (backlight, dimming, screen-off) is left alone. Call it before
 * power_init() so the first backlight write knows. */
void power_set_external(bool external);

/* What the battery says (the smoothed percentage included). */
const power_battery_t * power_battery(void);

/* Re-read the settings files (the Settings -> Power page changed one). */
void power_reload_settings(void);

/* Long tracks: X skips 30 s instead of a whole track when the current song
 * is longer than the setting. True when the skip should be a seek. */
bool power_skip_is_seek(void);
#define POWER_SEEK_STEP_S 30.0f

/* For the Settings page: a line like "78 %, charging". */
void power_battery_text(char * out, int len);

#endif
