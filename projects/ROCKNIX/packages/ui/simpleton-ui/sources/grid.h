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
#include "library.h"
#include "tilestyle.h"
#include <stdbool.h>

/* What the grid shows (0.19): every album (the Albums view), one folder's
 * children (an artist or library folder: album covers, and a name or
 * picture tile for a subfolder that isn't an album), or every track (the
 * Tracks view's grid). Set before grid_show(); the view's settings are
 * kept per scope kind (albums / folder / tracks) and per screen type. */
typedef enum {
    GRID_SCOPE_ALBUMS,
    GRID_SCOPE_FOLDER,
    GRID_SCOPE_TRACKS,
    GRID_SCOPE_ARTISTS,        /* 0.20: every artist (artists.h) - a picture or a name tile each */
    GRID_SCOPE_ARTIST,         /* 0.20: one artist's albums, `folder` = the artist's name       */
    GRID_SCOPE_COLLECTION      /* 0.21: one collection's albums, `folder` = its key (curation.h) */
} grid_scope_t;
void         grid_set_scope(grid_scope_t scope, const char * folder);
grid_scope_t grid_scope(void);
const char * grid_scope_folder(void);          /* NULL unless GRID_SCOPE_FOLDER */
const char * grid_scope_artist(void);          /* NULL unless GRID_SCOPE_ARTIST: the artist's name */
const char * grid_scope_collection(void);      /* NULL unless GRID_SCOPE_COLLECTION: the collection's key */
void         grid_curation_changed(void);      /* a collection changed (0.21) */

/* `open_folder`: Select on a folder tile (main.c opens that folder's own
 * view); `on_play`: a track was started from the Tracks grid (main.c shows
 * now-playing on the handheld). */
void grid_set_callbacks(void (*open_folder)(const char * uri), void (*on_play)(void), void (*on_style)(const char * style));
/* `open_artist`: Select on an artist tile (GRID_SCOPE_ARTISTS), the
 * artists.h index; main.c opens what the "Opens as" row says (0.20). */
void grid_set_artist_callback(void (*open_artist)(int artist));

/* Put the cursor on the tile whose key this is (an album's folder, a
 * folder's URI, a track's URI, an artist's name); false when it isn't in
 * the grid. */
bool grid_focus_key(const char * key);

/* `on_open` gets the album's library, its index in it, the card's cache
 * name, and the style its no-art tile was dealt (NULL for an album with
 * art) - the album page copies what it needs; the library may be reloaded
 * after. `on_exit` is Back out of the view. */
void grid_create(void (*on_open)(const library_t * lib, int album, const char * card, const tile_style_t * tile), void (*on_exit)(void));

/* Open the album whose folder this is, as Select would (the restart line
 * after an HDMI hotplug on the album page). False when it isn't in the
 * grid. */
bool grid_open_folder(const char * folder);

/* The album a folder holds, from the library index (Note 05 section 7:
 * a folder of one album opens as the album page from the browser too).
 * Loads the index if the grid hasn't yet. False when the folder isn't an
 * album folder of any card. `tile` gets the style its no-art tile was dealt
 * (NULL: with art, or not dealt yet). */
bool grid_lookup_folder(const char * uri, const library_t ** lib, int * album, const char ** card, const tile_style_t ** tile);
/* `fresh`: opened from Home (the first-visit panel or the pill may show);
 * false when coming back from an album (neither: the cursor is on it). */
void grid_show(bool fresh);
void grid_hide(void);

void grid_handle_action(ui_action_t a);
bool grid_overlay_active(void);        /* the picker panel or the pill is up */
void grid_toggle_picker(void);         /* the Settings button on the view   */
void grid_show_picker_style(void);     /* the panel, on its View row (a handover from the list page's panel, 0.19a) */

/* The view's settings files changed (Settings page). */
void grid_reload_options(void);

/* MPD's database changed: the index may be rebuilt soon; re-read it when
 * its file changes. */
void grid_library_changed(void);

#endif
