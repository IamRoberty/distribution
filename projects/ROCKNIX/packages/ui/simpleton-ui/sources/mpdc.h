/*
 * SimpletonOS UI - minimal MPD client.
 *
 * Talks the MPD text protocol over TCP to localhost:6600. Deliberately not
 * libmpdclient: the handful of commands the UI needs fit in a page, and it
 * keeps the package dependency graph flat. MPD is the control layer; this is
 * the thin translator the architecture note calls for.
 */
#ifndef SIMPLETON_MPDC_H
#define SIMPLETON_MPDC_H

#include <stdbool.h>

typedef enum { MPD_ENTRY_DIR, MPD_ENTRY_FILE } mpd_entry_kind_t;

typedef struct {
    mpd_entry_kind_t kind;
    char * uri;        /* MPD URI, relative to music_directory   */
    char * display;    /* what the list shows: title or basename */
} mpd_entry_t;

typedef struct {
    mpd_entry_t * items;
    int count;
} mpd_listing_t;

/* Try to connect (fast fail on localhost). Safe to call repeatedly; returns
 * true when a connection is up. */
bool mpd_connect(void);
bool mpd_is_connected(void);

/* Directory listing (`lsinfo`). Works on real directories and on container
 * files MPD knows how to expand (SACD ISO, CUE sheets). Returns false on
 * error; on success caller owns the listing and must free it. */
bool mpd_lsinfo(const char * uri, mpd_listing_t * out);
void mpd_listing_free(mpd_listing_t * l);

/* Replace the queue with the given URIs and start playing at start_index. */
bool mpd_play_uris(char * const * uris, int count, int start_index);

bool mpd_toggle_pause(void);
bool mpd_next(void);
bool mpd_previous(void);

#endif
