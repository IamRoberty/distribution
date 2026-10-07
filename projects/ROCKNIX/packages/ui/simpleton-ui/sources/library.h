/*
 * SimpletonOS - the library index: albums, artists, tracks and folder types
 * built from MPD's tags (0.12, 4 Oct 2026; Engineering Note 05 sections 7
 * and 8, build step 3b).
 *
 * The tag layer over MPD. One `listallinfo` per card gives every track's
 * tags in one round trip; this file groups them into albums and artists,
 * decides each folder's type, sorts the way the browse picker asks, and
 * reads / writes the whole thing as a small text file (library.idx) in the
 * cache folder so the UI can open it in a moment instead of asking MPD.
 * Every screen, play-through and the phone app see the same library
 * through here, the way the folder listing goes through
 * mpd_lsinfo_expanded().
 *
 * Rules (Ian + proposed, Note 05 section 8):
 *   - An album is the tracks sharing album artist (else artist) and album
 *     title, wherever they lie: a two-folder two-disc album is one album.
 *   - Tracks whose artists differ with no album artist tag are grouped by
 *     album title within one folder, and the album is marked `various`.
 *   - With no album tag, one folder is one album: its title is the folder
 *     name as it is, its artist is empty. Nothing is guessed from names.
 *   - Year: the original date first, then the date, first four digits.
 *   - A sort puts items lacking the tag it needs at the end, in folder
 *     order; an album with no year still sorts normally by title.
 *
 * No LVGL in here (the cache tool links it too).
 */
#ifndef SIMPLETON_LIBRARY_H
#define SIMPLETON_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    char *   uri;
    char *   title;            /* "" when untagged (the UI shows the number) */
    char *   artist;           /* track artist, "" when untagged              */
    int      track, disc;
    float    duration;
    long     mtime, added;     /* epoch seconds                               */
    int      album;            /* index into library_t.albums                 */
} lib_track_t;

typedef struct {
    char *   title;            /* album tag, or the folder name when untagged */
    char *   artist;           /* album artist, else the shared artist, else "" */
    char *   folder;           /* folder URI holding the (first) track        */
    char *   first_uri;        /* a track to look the cover up from           */
    int      year;             /* 0 = unknown                                 */
    long     newest;           /* latest `added` of its tracks (else mtime)   */
    int      first_track;      /* index of its first track in `tracks`        */
    int      track_count;
    bool     untagged;         /* no album tag: folder = album                */
    bool     various;          /* tracks by different artists, no album artist*/
    bool     in_collection;    /* lies in a collection folder (singles): the  */
                               /* Albums view may leave it out                */
    uint64_t fingerprint;      /* of its tracks' URIs and dates               */
    /* filled by the cache (0 / false until it has run) */
    uint32_t colour;           /* 0xRRGGBB preview of the cover, 0 = none     */
    bool     has_art;
    uint8_t  thumb_sizes;      /* bit i set = thumbnail lib_thumb_px[i] exists */
} lib_album_t;

typedef struct {
    char *   name;
    int      album_count, track_count;
} lib_artist_t;

/* Folder types (Note 05 section 7); thresholds in library.c. */
typedef enum {
    FOLDER_PLAIN = 0,          /* today's list                                 */
    FOLDER_ALBUM,              /* tracks of one album -> the album page        */
    FOLDER_COLLECTION,         /* many tracks, many artists -> list + jump strip */
    FOLDER_ARTIST,             /* album folders -> grid of covers              */
    FOLDER_LIBRARY,            /* artist folders -> artist tiles               */
} folder_type_t;

typedef struct {
    char *        uri;
    folder_type_t type;
    int           album;       /* FOLDER_ALBUM: which album; else -1          */
    int           tracks;      /* tracks directly inside                      */
    int           subfolders;  /* folders directly inside that hold music     */
} lib_folder_t;

typedef struct {
    char *         root;       /* the mount scanned, "card-<serial>" ("" = all) */
    lib_track_t *  tracks;   int ntracks;
    lib_album_t *  albums;   int nalbums;
    lib_artist_t * artists;  int nartists;
    lib_folder_t * folders;  int nfolders;
} library_t;

/* Thumbnail sizes the cache keeps per album (square, pixels). The grid picks
 * the first that is >= its cover size (layout.h). */
#define LIB_THUMB_COUNT 4
extern const int lib_thumb_px[LIB_THUMB_COUNT];      /* 192, 256, 384, 512 */

/* Build from MPD (one listallinfo of `root`). Albums come out in folder
 * order, tracks in MPD's order, folders typed. Returns false when MPD is
 * unreachable; `lib` is zeroed either way. */
bool library_build(library_t * lib, const char * root);

void library_free(library_t * lib);

/* The index file. */
bool library_save(const library_t * lib, const char * path);    /* written to path.tmp, then renamed */
bool library_load(library_t * lib, const char * path);

/* ---- sorts (the picker's rows) ---- */

typedef enum {
    LIB_SORT_TITLE = 0,        /* album title                                 */
    LIB_SORT_ARTIST_TITLE,     /* album artist, then title                    */
    LIB_SORT_ARTIST_YEAR,      /* album artist, then year                     */
    LIB_SORT_YEAR,             /* year, then artist, then title               */
    LIB_SORT_NEWEST,           /* newest on card first                        */
    LIB_SORT_COUNT
} lib_sort_t;

/* Fill `order` (nalbums entries) with album indices in sort order. Items
 * lacking what the sort needs come last, in folder order. */
void library_sort_albums(const library_t * lib, lib_sort_t sort, int * order);

/* Album indices of one artist (by name), in artist›year order; returns count. */
int library_albums_of_artist(const library_t * lib, const char * artist, int * out, int max);

/* The folder record for a URI, or NULL. */
const lib_folder_t * library_folder(const library_t * lib, const char * uri);

/* Case-insensitive, accent-insensitive-enough compare for sorting names:
 * Unicode-aware ordering arrives with the jump strip; this is byte order on
 * lower-cased ASCII, leading "The " ignored. */
int library_name_cmp(const char * a, const char * b);

const char * library_sort_name(lib_sort_t s);     /* "title" ... for files and logs */
const char * library_folder_type_name(folder_type_t t);

#endif
