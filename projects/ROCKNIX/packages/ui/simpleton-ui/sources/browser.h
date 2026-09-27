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

/* Build the browser on the given screen and bind it to the keypad group. */
void browser_create(lv_obj_t * screen, lv_group_t * group);

/* Handle a non-navigation action (BACK, MENU, ...). UP/DOWN/SELECT are
 * delivered through the LVGL keypad path and never come here. */
void browser_handle_action(ui_action_t a);

/* Periodic tick from the main loop: retries MPD when it isn't up yet. */
void browser_tick(void);

#endif
