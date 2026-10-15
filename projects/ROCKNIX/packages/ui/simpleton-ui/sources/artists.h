/*
 * SimpletonOS UI - the artists of the mounted cards (0.20, 14 Oct 2026;
 * Note 05 step 8, Ian's direction in browse-views-ian-2026-10-13.md).
 *
 * One list for the three Artists views (rows, grid, list): every album
 * artist of every card, merged by name across cards, each with their
 * albums in year order, and "Various Artists" last with the albums the
 * index marked `various`. Artists with no album of their own and only a
 * song or two in the library (a track on a compilation, a single in a
 * singles folder) stay out, as Ian asked ("continue to filter out artists
 * with only a handful of tracks").
 *
 * An artist's picture: the artist folder's own picture (artist.jpg,
 * folder.jpg ... - art_folder_image) when an album of theirs lies in a
 * folder named after them; else none, and the views draw a name tile.
 */
#ifndef SIMPLETON_ARTISTS_H
#define SIMPLETON_ARTISTS_H

#include "library.h"
#include <stdbool.h>

typedef struct { int src, album; } artist_album_t;

typedef struct {
    char *           name;
    artist_album_t * albums;     /* in year order, then title */
    int              nalbums;
    int              ntracks;
    char *           pic;        /* a picture file, or NULL */
    bool             various;    /* the "Various Artists" row */
} artist_t;

/* Rebuild from the mounted cards' indexes (libs.h) when they changed;
 * returns true when the list is new. */
bool artists_refresh(void);
int  artists_count(void);
const artist_t * artists_get(int i);
int  artists_find(const char * name);           /* -1 when not listed */
unsigned artists_generation(void);

/* "12 albums" / "1 album", or with `tracks` "84 tracks" / "1 track" - what
 * the views show under an artist (0.20c, Ian: when an artist opens as
 * tracks, the count is of tracks). Returns `out`. */
const char * artists_count_text(int i, bool tracks, char * out, size_t len);

/* The track URIs of one artist, in album (year) then track order; the
 * caller frees the array (not the strings). */
const char ** artists_track_uris(int i, int * count);

#endif
