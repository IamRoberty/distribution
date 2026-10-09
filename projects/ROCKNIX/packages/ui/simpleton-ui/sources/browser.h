/*
 * SimpletonOS UI - folder browser screen.
 *
 * The list is what MPD sees (lsinfo), not the raw filesystem: a SACD ISO or
 * a CUE-sheet album shows as its tracks, exactly as they will play.
 */
#ifndef SIMPLETON_BROWSER_H
#define SIMPLETON_BROWSER_H

#include "lvgl.h"
#include "input.h"
#include "layout.h"

/* Build the browser on the given screen and bind it to the keypad group.
 * The view's options (size, sort, jump strip) are read from the settings
 * files for this screen type. `on_play` is called after a track has been
 * started from a row; `on_exit` when Back is pressed at the top level
 * (0.13: main.c shows Home). `resume_uri` (may be NULL) opens that folder
 * instead of the top level. */
void browser_create(lv_obj_t * screen, lv_group_t * group, void (*on_play)(void), void (*on_exit)(void), const char * resume_uri);

/* The folder on screen ("" = top level), for handing over across a restart. */
const char * browser_current_uri(void);

/* The settings files for this view changed (the Settings page): re-read
 * them and apply what changed - size and sort on the spot, no restart. */
void browser_reload_options(void);

/* The Settings button on the view: the view picker panel, or away again (0.15). */
void browser_toggle_picker(void);

/* 0.16: the browser shares its picker with the grids; call when the browser
 * screen is shown again. A grid opens an album's folder straight in
 * (Back then leaves to the grid); Folders from Home goes back to the root. */
void browser_shown(void);
bool browser_open_folder(const char * uri);   /* false: the folder could not be read; nothing changed */
void browser_open_root(void);
bool browser_opened_at_folder(void);

/* The picker or the jump strip is up (0.14): every action, UP/DOWN/SELECT
 * included, must then come through browser_handle_action(). */
bool browser_overlay_active(void);

/* Handle a non-navigation action (BACK, MENU, ...). UP/DOWN/SELECT are
 * delivered through the LVGL keypad path and never come here - except
 * while the picker or strip is up (browser_overlay_active()). */
void browser_handle_action(ui_action_t a);

/* Periodic tick from the main loop: retries MPD when it wasn't up yet. */
void browser_tick(void);

/* MPD said the library changed (`db_changed`: a rescan finished with new
 * contents) or a rescan started/stopped. Reloads the folder on screen,
 * keeping the focused row; if the folder is gone (card pulled), climbs to the
 * nearest one that exists. An empty list reads "Scanning library..." while a
 * rescan runs. Driven by MPD's notifications, never by polling. */
void browser_library_changed(bool db_changed);

#endif
