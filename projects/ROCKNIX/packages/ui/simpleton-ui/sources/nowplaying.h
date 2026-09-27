/*
 * SimpletonOS UI - now-playing screen.
 *
 * Full-bleed album art on the 720x720 panel, with the controls floating on
 * a translucent strip over the bottom edge. Three focusable rows (settled
 * design, ui-design.md): scrub bar, transport, and heart / info / options.
 * The strip fades out after a few seconds; with it hidden, the device is
 * just the album cover. Focus defaults to the transport row on entry.
 *
 * Actions are delivered directly by main.c while this screen is active;
 * nothing here goes through LVGL's keypad group.
 */
#ifndef SIMPLETON_NOWPLAYING_H
#define SIMPLETON_NOWPLAYING_H

#include "lvgl.h"
#include "input.h"

typedef enum { NP_HANDLED, NP_EXIT } np_result_t;

/* Build the screen (not loaded yet). Starts the art worker. */
void nowplaying_create(void);

/* Load the screen: controls shown, transport row focused, status refreshed. */
void nowplaying_show(void);

/* Called by main.c after it has loaded another screen. */
void nowplaying_hide(void);

/* Handle any action while active. NP_EXIT = user backed out to the browser;
 * the caller switches screens. */
np_result_t nowplaying_handle_action(ui_action_t a);

#endif
