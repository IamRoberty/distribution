/*
 * SimpletonOS UI - the Settings hub (0.13, 7 Oct 2026; Engineering Note 05
 * section 2, build step 4: the shell).
 *
 * One hub page lists the general groups - Playback, Library, Display &
 * appearance, Controls, System - and then one page per view (Folders,
 * Albums, ...), the same pages the browse picker will open, so no setting
 * has to be remembered by location (Ian). The settings menu button (314)
 * on the handheld opens the page of the view on screen; that page ends
 * with "All settings", which leads to the hub. Back walks the same way
 * out.
 *
 * What a page holds today is what the player already has a setting for:
 * play-through, scan on insert, cache on card, a library scan, the theme,
 * the language, and the Folders view's options (view style, size, sort,
 * jump strip - the picker's rows, 0.14). The groups are all there so the
 * shape of the menu is settled; empty ones say so.
 *
 * Every setting is one file (config.h). A change that the running UI
 * can't take on the fly (theme, language) is applied by restarting the
 * UI on the same page - the same under-a-second restart the HDMI hotplug
 * uses. A view's options are handed to the view live.
 *
 * Shared screen: theme colours and fonts only. Takes every action
 * directly from main.c, like now-playing.
 */
#ifndef SIMPLETON_SETTINGS_H
#define SIMPLETON_SETTINGS_H

#include "input.h"
#include "layout.h"
#include <stdbool.h>

/* `restart` is called to apply a setting that needs a fresh start; it
 * receives the page to come back to. `view_changed` is called with a
 * view's id when one of that view's options (size, sort, jump strip)
 * changed, so the view can take it on the spot (0.14). */
void settings_create(void (*restart)(const char * page), void (*view_changed)(const char * view));

/* Load the screen on `page`: NULL or "hub" for the hub, a group ("playback",
 * "library", "display", "controls", "system") or a view id ("folders" ...).
 * `from_view` true: opened from a view with the menu button, so Back on
 * that page leaves Settings instead of climbing to the hub. */
void settings_show(const char * page, bool from_view);
void settings_hide(void);

typedef enum { SET_HANDLED, SET_EXIT } set_result_t;
set_result_t settings_handle_action(ui_action_t a);

/* MPD said the library changed: the scan row's state. */
void settings_mpd_changed(void);

#endif
