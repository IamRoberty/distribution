/*
 * SimpletonOS UI - the Home page (0.13, 7 Oct 2026; Engineering Note 05
 * section 2, build step 4).
 *
 * The first screen, and the one every view's Back leads to. It lists the
 * places to go: Folders, Albums, Artists, Tracks, Recent, Favorites,
 * Mixtapes (playlists), Collections, Visualization, Now Playing (only while
 * something is playing) and Settings. The page belongs to the THEME: the
 * Archer Ave. theme draws it as a shelf of cassette tapes (shelf.h) in the
 * order its .theme file lists them; a theme with no shelf, or one whose
 * art can't be read, gets a plain list of the same entries, so the page
 * works before any art does and never waits for it.
 *
 *   home shelf                                      # the theme draws Home as a shelf
 *   shelf rest 0.889 / pull 0.96 / rail 60 / back 1A100C / shadow 40
 *   shelf wood-h <file.png> / wood-v <file.png>     # tiles for the rails
 *   tape <entry> <tile.png> <font.ttf> <pt> <case> <RRGGBB> [start|centre|end]
 *
 * `entry` is one of the ids below; `mixtapes` is the playlists view under
 * its Archer Ave. name. The label is the string table's word for the
 * entry, so it follows the language setting; the theme only says how it
 * is written (typeface, size in points at the 1080 master, case, ink).
 *
 * Like now-playing, this screen takes every action directly from main.c.
 */
#ifndef SIMPLETON_HOME_H
#define SIMPLETON_HOME_H

#include "lvgl.h"
#include "input.h"
#include <stdbool.h>

typedef enum {
    HOME_FOLDERS, HOME_ALBUMS, HOME_ARTISTS, HOME_TRACKS, HOME_RECENT, HOME_FAVORITES,
    HOME_PLAYLISTS, HOME_COLLECTIONS, HOME_VISUALIZATION, HOME_NOW_PLAYING, HOME_SETTINGS,
    HOME_VIEW_COUNT
} home_view_t;

/* Build the page (not loaded yet). `on_open` is called with the view the
 * user selected; main.c switches screens. */
void home_create(void (*on_open)(home_view_t view));

void home_show(void);
void home_hide(void);

/* Every action while the page is active (Back does nothing here: Home is
 * the root). */
void home_handle_action(ui_action_t a);

/* MPD said playback changed: the Now Playing tape appears while something
 * is playing or paused and goes away when nothing is. */
void home_mpd_changed(void);

/* The entry's id ("folders"), as the theme file and the settings use it,
 * and its translated name. */
const char * home_view_id(home_view_t v);
const char * home_view_name(home_view_t v);

/* The view with this id, or HOME_VIEW_COUNT. "mixtapes" is HOME_PLAYLISTS. */
home_view_t home_view_from_id(const char * id);

#endif
