/*
 * SimpletonOS UI - the album grid (0.16, 7 Oct 2026; Engineering Note 05
 * section 4 and the 7 Oct amendments, build step 6a).
 *
 * The Albums view: every album of every card on the unit, as covers in a
 * grid, from the library index and the thumbnails the cache keeps
 * (library.h, cache.h, thumbs.h). Theme-owned in principle; this first
 * build draws with the theme's colours and fonts and no theme artwork.
 *
 *   Sizes    rows per screen from layout.h: 1x1 .. 4x4 on the handheld
 *            (1 across is for flipping through a small Collection one
 *            cover at a time), 1x2 .. 5x10 on a TV. Default L.
 *   D-pad    Up/Down between rows, scrolling a row at the edges; Left/Right
 *            along a row, and off the edge a page back or forward, the
 *            cursor keeping its place. The ends stop.
 *   Focus    the selected cover comes forward (115%, a shadow, drawn over
 *            its neighbours) in a ~100 ms zoom, and is fetched sharper.
 *   Names    none on the covers. On the handheld a bar along the bottom
 *            shows "Artist - Album" for a moment as the cursor moves; on a
 *            TV the info band at the top shows it always.
 *   No art   a tile in a tape colourway (background = the label colour,
 *            ink = the text colour) with the album title in the tape's
 *            album font, and the artist above it when the tile is big
 *            enough. The colourway is random per visit and held while in
 *            the grid, so paging back shows the same colours.
 *   Loading  every tile shows its album's colour preview at once and
 *            sharpens as its thumbnail is decoded (thumbs.h); the page
 *            ahead in the direction of travel is decoded next.
 *   Picker   the shared picker (picker.h): Grid style, size, sort (title,
 *            artist-title, artist-year, year, newest on card), alphabet
 *            picker on/off; remembered per screen type in
 *            <what>-albums-<kind>. The pill jumps by letter (or by decade
 *            in the year sorts), sitting over the top row of covers.
 *   Select   opens the album (the album page in step 6b; until then the
 *            album's folder in the browser, from which Back returns here).
 *   Back     leaves to Home (two presses from a long grid: the pill first,
 *            as in the browser).
 */
#ifndef SIMPLETON_GRID_H
#define SIMPLETON_GRID_H

#include "lvgl.h"
#include "input.h"
#include <stdbool.h>

/* `on_open` gets the album's folder URI and its first track's URI;
 * `on_exit` is Back out of the view. */
void grid_create(void (*on_open)(const char * folder_uri, const char * first_uri), void (*on_exit)(void));
/* `fresh`: opened from Home (the first-visit panel or the pill may show);
 * false when coming back from an album (neither: the cursor is on it). */
void grid_show(bool fresh);
void grid_hide(void);

void grid_handle_action(ui_action_t a);
bool grid_overlay_active(void);        /* the picker panel or the pill is up */
void grid_toggle_picker(void);         /* the Settings button on the view   */

/* The view's settings files changed (Settings page). */
void grid_reload_options(void);

/* MPD's database changed: the index may be rebuilt soon; re-read it when
 * its file changes. */
void grid_library_changed(void);

#endif
