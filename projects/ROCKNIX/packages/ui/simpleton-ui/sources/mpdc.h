/*
 * SimpletonOS UI - minimal MPD client.
 *
 * Talks the MPD text protocol over TCP to localhost:6600. Deliberately not
 * libmpdclient: the handful of commands the UI needs fit in a page, and it
 * keeps the package dependency graph flat. MPD is the control layer; this is
 * the thin translator the architecture note calls for.
 *
 * Threading: there are three independent connections. Everything below except
 * mpd_readpicture() runs on the UI thread over the main connection. The art
 * loader's worker thread calls mpd_readpicture() only, over its own second
 * connection, so the two never interleave on one socket.
 *
 * The third connection (UI thread too) sits in MPD's `idle` state: MPD sends
 * nothing on it until something changes, then names what changed. The main
 * loop poll()s its fd, so the UI learns about a finished rescan or a track
 * change the moment it happens, with no periodic polling of MPD at all.
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
    char * section;    /* divider label to show above this entry ("Disc 1"), or NULL */
    long   mtime;      /* Last-Modified as epoch seconds, 0 when MPD gave none (0.14) */
} mpd_entry_t;

typedef struct {
    mpd_entry_t * items;
    int count;
} mpd_listing_t;

/* Snapshot of `status` + `currentsong`. Strings are empty when unknown. */
typedef struct {
    char  state[8];        /* "play", "pause", "stop"                    */
    int   songid;          /* -1 when nothing is current                 */
    int   song;            /* queue position of the current song, -1     */
    int   nextsong;        /* queue position MPD will play next, -1 when */
                           /* the current song is the last one           */
    int   playlistlength;  /* songs in the queue                         */
    bool  repeat, random, single;   /* playback options; single covers   */
                                    /* "oneshot" too                     */
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

/* The folder as the product shows it (1 Oct 2026, "transparent containers"):
 * `lsinfo`, with every container MPD expands (SACD ISO, multi-track DFF, a
 * CUE album, a FLAC with an embedded cue sheet) replaced in place by the
 * tracks inside it, so an album never shows a file you have to open first.
 * The audio file a CUE sheet describes is hidden behind the sheet's tracks.
 * Two discs in one folder simply run on, no divider (`section` is reserved).
 * Every other client (play-through, the phone app later) must use this, not
 * raw lsinfo, so "the tracks of a folder" mean the same thing everywhere. */
bool mpd_lsinfo_expanded(const char * uri, mpd_listing_t * out);

/* True when a path is a container file MPD presents as a directory
 * (by name: .iso, .dff, .cue, ...). */
bool mpd_is_container_name(const char * uri);

/* Replace the queue with the given URIs and start playing at start_index. */
bool mpd_play_uris(char * const * uris, int count, int start_index);

/* Append URIs to the end of the queue without touching playback. */
bool mpd_add_uris(char * const * uris, int count);

/* One song as `listallinfo` describes it (0.12, the library index). Fixed
 * buffers, valid only inside the callback. Times are epoch seconds (UTC);
 * `added` is when MPD first saw the song (MPD 0.24), `mtime` the file's date. */
typedef struct {
    char  uri[1024];
    char  title[256], artist[256], album_artist[256], album[256];
    char  date[32], original_date[32];
    int   track, disc;
    float duration;
    long  mtime, added;
} mpd_song_info_t;

/* Stream every song under `uri` ("" = whole library) through `cb`, in MPD's
 * order (depth first, as the folders lie). Return false from `cb` to stop
 * early (the rest of the reply is still drained). Returns false when MPD is
 * unreachable or refuses. One round trip whatever the library size; the
 * reply is parsed line by line, never held whole. */
bool mpd_listallinfo(const char * uri, bool (*cb)(const mpd_song_info_t * song, void * ctx), void * ctx);

/* Library state from `status` + `stats`: `db_update` is MPD's timestamp of
 * the last finished database update (changes every time a rescan completes),
 * `updating` is true while a rescan is running. Returns false when MPD is
 * unreachable. */
bool mpd_library_state(long * db_update, bool * updating);

/* Ask MPD to rescan the whole library (incremental; `update`). The result
 * arrives as database notices. */
bool mpd_update(void);

/* ---- change notifications (MPD `idle`) ---- */

/* What changed, as reported by MPD. */
#define MPD_CHANGED_DATABASE  (1u << 0)   /* a library update finished and changed the DB */
#define MPD_CHANGED_UPDATE    (1u << 1)   /* a library update started or finished          */
#define MPD_CHANGED_PLAYER    (1u << 2)   /* play / pause / stop / seek / track change     */
#define MPD_CHANGED_MIXER     (1u << 3)   /* volume                                        */
#define MPD_CHANGED_OPTIONS   (1u << 4)   /* repeat / random / single / consume            */
#define MPD_CHANGED_PLAYLIST  (1u << 5)   /* the queue                                     */
#define MPD_CHANGED_OUTPUT    (1u << 6)   /* an audio output was enabled / disabled        */
#define MPD_CHANGED_ALL       0x7Fu
#define MPD_IDLE_LOST         (1u << 31)  /* connection dropped (MPD restarted)            */

/* Open the idle connection if it isn't open. Returns true only when it was
 * newly opened, so the caller knows to treat everything as changed (nothing
 * was being watched while it was down). Cheap no-op while connected. */
bool mpd_idle_start(void);

/* Its fd for poll() (POLLIN), or -1 while disconnected. */
int mpd_idle_fd(void);

/* Call when the fd is readable: reads what changed, re-arms the watch and
 * returns MPD_CHANGED_* bits (or MPD_IDLE_LOST, after which the fd is -1
 * until mpd_idle_start() succeeds again). */
unsigned mpd_idle_poll(void);

bool mpd_toggle_pause(void);
bool mpd_resume(void);      /* play: unpause, or start the queue if stopped */
bool mpd_pause(void);       /* pause if playing; no-op otherwise            */
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
