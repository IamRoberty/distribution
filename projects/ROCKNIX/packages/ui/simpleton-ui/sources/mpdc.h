/*
 * SimpletonOS UI - minimal MPD client.
 *
 * Talks the MPD text protocol over TCP to localhost:6600. Deliberately not
 * libmpdclient: the handful of commands the UI needs fit in a page, and it
 * keeps the package dependency graph flat. MPD is the control layer; this is
 * the thin translator the architecture note calls for.
 *
 * Threading: there are two independent connections. Everything below except
 * mpd_readpicture() runs on the UI thread over the main connection. The art
 * loader's worker thread calls mpd_readpicture() only, over its own second
 * connection, so the two never interleave on one socket.
 */
#ifndef SIMPLETON_MPDC_H
#define SIMPLETON_MPDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Snapshot of `status` + `currentsong`. Strings are empty when unknown. */
typedef struct {
    char  state[8];        /* "play", "pause", "stop"                    */
    int   songid;          /* -1 when nothing is current                 */
    float elapsed;         /* seconds                                    */
    float duration;        /* seconds, 0 when unknown (streams)          */
    char  audio[32];       /* decoded format as MPD reports it:          */
                           /*   "44100:16:2", "96000:24:2", "dsd64:2"    */
    char  file[1024];      /* URI of the current song                    */
    char  title[256];
    char  artist[256];
    char  album[256];
} mpd_status_t;

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

/* Relative seek within the current song (`seekcur +N` / `seekcur -N`). */
bool mpd_seek_relative(float seconds);

/* Fill `out` from `status` and `currentsong`. Returns false when MPD is
 * unreachable; `out` is zeroed either way, so callers can render it. */
bool mpd_status(mpd_status_t * out);

/* Fetch the picture embedded in a song's tags (`readpicture`). Runs on the
 * ART WORKER THREAD over its own connection - never call from the UI thread.
 * Returns true with a malloc'd buffer the caller frees; false when the song
 * has no embedded picture or MPD is unreachable. */
bool mpd_readpicture(const char * uri, uint8_t ** data, size_t * len);

#endif
