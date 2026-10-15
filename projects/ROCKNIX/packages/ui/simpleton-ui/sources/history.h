/*
 * SimpletonOS UI - what was played, most recent first (0.19a, 13 Oct 2026).
 *
 * The Recent view lists the tracks last played, newest at the top (Ian, 13
 * Oct: "we want the most recently played tracks"). MPD keeps no such
 * record, so the UI keeps one: a track is noted ten seconds after it
 * becomes the current song and is still the current song then - a skip
 * through three tracks notes none of them. A track played again moves to
 * the top; the list holds the last HISTORY_MAX. Kept in a plain text file,
 * one URI per line, newest first, written through a temporary name.
 *
 * No LVGL in here apart from the timer; nothing else uses it.
 */
#ifndef SIMPLETON_HISTORY_H
#define SIMPLETON_HISTORY_H

#include <stdbool.h>

#define HISTORY_MAX 500

/* Load the file; `changed` is called (on the UI thread) after each note. */
void history_init(void (*changed)(void));

/* A player notice from MPD: look at the current song. */
void history_mpd_changed(void);

int          history_count(void);
const char * history_get(int i);        /* 0 = the most recent */
unsigned     history_generation(void);  /* bumped on every change */

/* The file's path (for tests and the docs): $SIMPLETON_HISTORY, else
 * <config dir>/history. */
const char * history_path(void);

#endif
