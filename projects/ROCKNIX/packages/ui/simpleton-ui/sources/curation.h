/*
 * SimpletonOS UI - favourites, mixtapes and collections (0.21, 15 Oct 2026;
 * Note 05 step 9, curation-0.21.md).
 *
 * Kept on the music card (Ian, 14 Oct: "that way favorites and collections
 * will persist from unit to unit, and also software updates ... won't wipe
 * out people's collections and playlists"), in the card's hidden folder
 * beside the library cache, as plain files any player or PC can read:
 *
 *   <card>/.simpleton/curation/favorites.m3u8          favourite tracks, oldest first
 *                              mixtapes/m0001.m3u8     a mixtape: #PLAYLIST:<name>, its tracks in order
 *                              collections/c0001.txt   a collection: #NAME:<name>, one album a line
 *
 * Track lines are paths relative to the playlist file ("../../Music/..."),
 * so a PC player opening the playlist off the card finds the music; a line
 * from a playlist made elsewhere is read relative to the card's root after
 * any "../", "./" or drive letter. A collection line is the album's folder
 * (from the card's root), its artist and its title, tab-separated.
 * Display names live inside the files; the file names are safe ids.
 * "#SIMPLETON-SEQ:n" orders mixtapes and collections, most recently
 * changed first, whatever the clock says.
 *
 * Every save writes the whole file under a temporary name, flushes it to
 * the card, keeps the old one as .bak and renames the new one in; a card
 * pulled mid-save keeps the last good version (the loader takes the file,
 * else .tmp, else .bak). Every save is mirrored to the unit
 * (/storage/.simpleton/curation/<card>/); a card whose curation folder is
 * missing altogether gets this unit's copy back. A card that can't be
 * written keeps the change on screen until it is pulled; the caller tells
 * the user.
 *
 * Every mounted card's items show together; a card that is out takes its
 * own with it. The store follows libs.h: a new library generation re-reads
 * the cards. No LVGL in here.
 */
#ifndef SIMPLETON_CURATION_H
#define SIMPLETON_CURATION_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { CUR_MIXTAPE, CUR_COLLECTION } cur_kind_t;

typedef struct {
    int        uid;          /* stable for the session (the views and the menu hold it) */
    cur_kind_t kind;
    char       id[64];       /* "m0001" / "c0001": the file's name (a playlist dropped in from a PC keeps its own) */
    char *     name;
    char       card[64];     /* the card that keeps it */
    unsigned   seq;          /* bumped on every change */
    int        n, cap;
    char **    uris;         /* mixtape: track URIs ("card-x/..."); collection: album folder URIs */
    char **    titles;       /* collection: the albums' titles (NULL for a mixtape) */
    char **    artists;      /* collection: the albums' artists */
} cur_list_t;

/* Bumped whenever anything changes (a card read, an edit). */
unsigned curation_generation(void);

/* ---- favourites ---- */
bool         curation_is_favorite(const char * uri);
int          curation_favorite_count(void);
const char * curation_favorite(int i);          /* oldest first, card by card */
/* Turn a track's heart on or off. False when it couldn't be saved to the
 * card (the change still shows until the card is pulled) or the track's
 * card isn't mounted. */
bool         curation_set_favorite(const char * uri, bool on);

/* ---- mixtapes and collections ---- */
int                curation_count(cur_kind_t k);
const cur_list_t * curation_get(cur_kind_t k, int i);   /* most recently changed first */
const cur_list_t * curation_by_uid(int uid);
/* "<card>/<id>" (the restart arguments, the grid's scope) <-> uid; 0 = none. */
int                curation_find_key(const char * key);
void               curation_key(int uid, char * out, size_t len);
bool               curation_name_used(cur_kind_t k, const char * name);
/* A new, empty list on `card`; its uid, or 0 when that card isn't mounted
 * or the save failed outright (no folder could be made). */
int                curation_new(cur_kind_t k, const char * card, const char * name);
/* Add tracks to a mixtape, skipping those already on it; `*added` gets the
 * count added. False: not saved to the card. */
bool               curation_add_tracks(int uid, const char * const * uris, int n, int * added);
/* Add an album to a collection unless it is there; `*added` says which. */
bool               curation_add_album(int uid, const char * folder, const char * title, const char * artist, bool * added);
bool               curation_contains_album(int uid, const char * folder, const char * title);
bool               curation_rename(int uid, const char * name);   /* 0.21a: the keyboard */
bool               curation_remove_at(int uid, int pos);
bool               curation_delete(int uid);

/* The card a URI lives on ("card-xxx"), in `out`; "" when it has none. */
const char *       curation_card_of(const char * uri, char * out, size_t len);

/* Paths, for the tests and the docs. */
void               curation_dir(const char * card, char * out, size_t len);
void               curation_mirror_dir(const char * card, char * out, size_t len);

#endif
