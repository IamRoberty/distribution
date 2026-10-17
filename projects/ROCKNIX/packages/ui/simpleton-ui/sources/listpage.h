/*
 * SimpletonOS UI - the list page (0.19, 13 Oct 2026; Note 05 steps 7–8,
 * Ian's direction of 13 Oct in browse-views-ian-2026-10-13.md; reshaped
 * in 0.19a after his first pass on the unit the same evening).
 *
 * The long-list cousin of the album page: Tracks (every track of every
 * card), Recent (what was played last, newest first - history.h), a mixed
 * folder of singles (a collection folder, which looks like a mixtape), one
 * album of many artists (a soundtrack: it looks the same way), and a
 * folder's albums as a list (the folder grid's List style). Thousands of
 * rows, so only the rows near the screen exist as objects; every row is
 * one height - the title on one line, the artist under it, in Jost.
 *
 *   TV         the list beside a 9/16 square that shows the selected
 *              item's cover once the cursor settles; a track without art
 *              shows its album as a name tile, a cassette or a record (one
 *              of the three per album, in the section's colours); in a
 *              mixtape-style folder or album, the cassette with the
 *              folder's name on its label. Left/Right page up and down.
 *   Handheld   "Tiles": four rows with the album's 192 px thumbnail (or
 *              its small name tile) at the left; "List": seven plain
 *              rows; Tracks also has "Grid": two columns of four text
 *              cells, title and artist, no art (Ian: art alone doesn't
 *              say which song it is).
 *   Colours    a colourway per letter (year, or artist's letter) of the
 *              sort, dealt from the theme's colour sets at the theme's
 *              listcontrast floor - one colour saturated, the other
 *              light, never black on white. The page switches colourway
 *              as the cursor crosses into the next letter; no fade.
 *   Jump row   TV only: "<< R >>" pinned above the list, the stop the
 *              cursor is in. Up from the top row on screen selects it;
 *              on it, Left/Right and Select jump to the previous/next
 *              stop (the arrow flashes in the playing colour) and the
 *              list moves under it; Down goes back into the list. Stops
 *              are letters, split at the second letter so one stop is
 *              about five pages ("Q, R, Ro, S, Se, Sl"), or years; a
 *              letter with less than a screenful joins its neighbour.
 *              The handheld has no jump row: its picker opens on "Jump
 *              to", which is the same thing in two presses.
 *   Picker     view style, size, sort, the alphabet switch, and - first,
 *              and focused when it opens - a "Jump to" row.
 *   Select     plays the track and the list after it, in order, whatever
 *              the play-through-folders setting (playthrough's list
 *              mode); an album opens its page, a folder its view.
 *   Back       leaves (Home, or where the folder was opened from).
 */
#ifndef SIMPLETON_LISTPAGE_H
#define SIMPLETON_LISTPAGE_H

#include "lvgl.h"
#include "input.h"
#include "library.h"
#include "tilestyle.h"
#include <stdbool.h>

typedef enum {
    LIST_TRACKS,      /* every track of every card                        */
    LIST_RECENT,      /* what was played last, newest first               */
    LIST_FOLDER,      /* a collection folder's tracks (mixtape-style)      */
    LIST_ALBUM,       /* one album's tracks, many artists (mixtape-style)  */
    LIST_ALBUMS,      /* a folder's albums and subfolders, as rows         */
    LIST_ARTISTS,     /* 0.20: every artist (artists.h); on a TV the square is a mosaic of their albums */
    LIST_ARTIST_TRACKS, /* 0.20: one artist's tracks, by title or in album order */
    LIST_FAVORITES,   /* 0.21: the favourite tracks of every card, in the order added */
    LIST_MIXTAPES,    /* 0.21: every mixtape, a row each (a tape on the TV square) */
    LIST_MIXTAPE,     /* 0.21: one mixtape's tracks (mixtape-style) */
    LIST_COLLECTIONS, /* 0.21: every collection, a row each (a mosaic on the TV square); one opens as the grid */
    LIST_KIND_COUNT
} list_kind_t;

/* `on_exit`: Back. `on_play`: a track was started (the handheld shows
 * now-playing). `on_style`: the picker chose a style this page can't draw
 * ("grid" for a folder's albums): main.c switches to the grid with the
 * same scope. */
void listpage_create(void (*on_exit)(void), void (*on_play)(void), void (*on_style)(const char * style));

/* Rows that open something (0.19a): an album row (LIST_ALBUMS) opens the
 * album the way the grid does; a folder row opens the folder's view. */
void listpage_set_callbacks(void (*open_album)(const library_t *, int album, const char * card, const tile_style_t * tile),
                            void (*open_folder)(const char * uri));
/* An artist row (LIST_ARTISTS) opens what the "Opens as" row says (main.c). */
void listpage_set_artist_callback(void (*open_artist)(int artist));
/* A mixtape or collection row (0.21) opens it: main.c shows the mixtape's
 * list or the collection's grid. */
void listpage_set_list_callback(void (*open_list)(int uid));

/* Set what the page lists; `folder` for LIST_FOLDER and LIST_ALBUMS. The
 * rows are built from the mounted cards' indexes (libs.h) at show. False
 * only for a folder that no index knows. */
bool listpage_open(list_kind_t kind, const char * folder);

/* One album's tracks (LIST_ALBUM): the album of `card`'s index. */
bool listpage_open_album(const char * card, int album);

/* One artist's tracks (LIST_ARTIST_TRACKS, 0.20): `by_album` starts the
 * list in album (year) then track order, else by title; the picker's Sort
 * row can change it after. The artist is the artists.h index. */
bool listpage_open_artist_tracks(int artist, bool by_album);
int  listpage_artist(void);                  /* LIST_ARTIST_TRACKS: the artist; -1 otherwise */

/* One mixtape's tracks (LIST_MIXTAPE, 0.21): the curation.h uid. */
bool listpage_open_mixtape(int uid);
int  listpage_mixtape(void);                 /* LIST_MIXTAPE: the uid; 0 otherwise */

list_kind_t  listpage_kind(void);
/* Put the cursor on the row whose key this is (a track's URI, an album's
 * folder, a folder's URI); false when it isn't in the list. */
bool listpage_focus_key(const char * key);
const char * listpage_folder(void);          /* the folder (LIST_FOLDER / LIST_ALBUMS) or the album's folder (LIST_ALBUM); NULL otherwise */

/* `fresh`: a new visit (colourways dealt again, the first row selected);
 * false coming back from now-playing or Settings. */
void listpage_show(bool fresh);
void listpage_hide(void);

void listpage_handle_action(ui_action_t a);
bool listpage_overlay_active(void);
void listpage_toggle_picker(void);
/* Open the picker on its View row (a handover from another view's picker, 0.19a). */
void listpage_show_picker_style(void);
void listpage_reload_options(void);
void listpage_library_changed(void);
void listpage_history_changed(void);
void listpage_curation_changed(void);        /* a favourite, mixtape or collection changed (0.21) */
void listpage_mpd_changed(void);

/* The settings id the page keeps its options under ("tracks", "recent",
 * "mixtape", "folder", "artists", "artist-tracks", "favorites", "mixtapes",
 * "collections"). */
const char * listpage_view_id(void);

#endif
