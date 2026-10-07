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
 * `size` is the list size (the Folders view's setting on this screen).
 * `on_play` is called after a track has been started from a row.
 * `resume_uri` (may be NULL) opens that folder instead of the top level. */
void browser_create(lv_obj_t * screen, lv_group_t * group, ui_size_t size, void (*on_play)(void), const char * resume_uri);

/* The folder on screen ("" = top level), for handing over across a restart. */
const char * browser_current_uri(void);

/* True at the top level: Back from here leaves the view (0.13: to Home). */
bool browser_at_root(void);

/* Handle a non-navigation action (BACK, MENU, ...). UP/DOWN/SELECT are
 * delivered through the LVGL keypad path and never come here. */
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
